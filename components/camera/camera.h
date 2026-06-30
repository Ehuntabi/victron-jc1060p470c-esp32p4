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

/* Genera un BMP de 8/24 bits del ultimo frame en un buffer recien reservado
 * (PSRAM). El que llama debe hacer free(*out). false si aun no hay frame. */
bool camera_snapshot_bmp(uint8_t **out, size_t *out_len);

/* Codifica el ultimo frame a JPEG por HW (recorte 960x544). Devuelve un puntero
 * a un buffer PERSISTENTE interno (NO hacer free) y su tamano. false si no hay
 * frame o falla el encoder. Salida ~80KB -> escritura corta que no choca con la
 * camara en el bus de la SD (a diferencia del BMP de 1.5MB). */
bool camera_snapshot_jpeg(uint8_t **out, size_t *out_len);

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
