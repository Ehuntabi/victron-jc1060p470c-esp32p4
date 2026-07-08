/* Servicio de camara MIPI-CSI (OmniVision OV02C10). Hito 1: bring-up + medir luz.
 * Encapsula esp_video/esp_cam_sensor. El resto del firmware no conoce el pipeline CSI.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa la camara OV02C10 por MIPI-CSI reutilizando el bus I2C dado
 * (el del proyecto, GPIO 7/8, compartido con touch/RTC). Auto-detecta el sensor
 * y arranca una tarea de streaming continuo (mide luminosidad).
 * Devuelve ESP_OK si esp_video_init() y la deteccion del sensor van bien.
 * Llamar DESPUES de bsp_i2c_init() (el handle no puede ser NULL).
 */
esp_err_t camera_init(i2c_master_bus_handle_t i2c);

/* Luminosidad ambiente media del ultimo frame (0-255, suavizada). Devuelve
 * false si aun no hay frame valido. Base del auto-brillo. */
bool camera_get_luma(uint8_t *out_luma);

/* Cerrojo de bus camara<->SD: los escritores de SD (datalogger/battery_history/log)
 * DEBEN envolver su I/O con estas para no solapar con el DMA de la camara (la
 * contencion en el controlador SDMMC provoca INT WDT -> reinicio). lock devuelve
 * false si no consigue el bus en timeout_ms (entonces omitir la escritura y reintentar
 * luego). Si la camara no esta arrancada (mutex no creado) permite siempre. */
bool camera_sd_bus_lock(uint32_t timeout_ms);
void camera_sd_bus_unlock(void);

/* Codifica el ultimo frame a JPEG por HW (recorte 960x544). THREAD-SAFE (mutex del
 * encoder). Devuelve una COPIA nueva en PSRAM: el que llama hace free(*out). false
 * si no hay frame o falla el encoder. Salida ~80-150KB. */
bool camera_snapshot_jpeg(uint8_t **out, size_t *out_len);

/* Decodifica un JPEG a un buffer RGB565 en PSRAM (para mostrarlo en un lv_img).
 * THREAD-SAFE (mutex del codec). El que llama hace free(*out). Deja en *out_w el
 * paso de fila (ancho alineado a 16) y en *out_h el alto real. false si falla. */
bool camera_decode_jpeg_rgb565(const uint8_t *jpg, size_t len,
                               uint8_t **out, int *out_w, int *out_h);

/* Codifica un framebuffer RGB565 (w%16==0, h%8==0) a JPEG por HW. THREAD-SAFE.
 * El que llama hace free(*out). Usado por la captura del carrusel (mucho mas
 * pequeno y rapido que el BMP). false si el tamano no cumple o falla. */
bool camera_encode_rgb565_jpeg(const uint16_t *rgb, int w, int h, int quality,
                               uint8_t **out, size_t *out_len);

/* Activa/desactiva el modo vigilancia: con on=true la tarea de camara detecta
 * movimiento y guarda las fotos JPEG en un anillo en RAM (no SD; el bus SDMMC se
 * satura con la camara+C6). Se ven por HTTP en /vigilancia. Lo llama el modo ausente. */
void camera_set_surveillance(bool on);

/* Anillo de capturas de vigilancia en RAM (servido por el HTTP /vigilancia). */
#include <time.h>
/* Lista las capturas (mas nueva primero). Rellena ids/ts/lens hasta max -> count. */
int  camera_vig_list(uint32_t *ids, time_t *ts, size_t *lens, int max);
/* Copia el JPEG de la captura 'id' a un buffer nuevo (caller hace free). false si rotada. */
bool camera_vig_fetch(uint32_t id, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
