/* Servicio de camara MIPI-CSI (OmniVision OV02C10). Hito 1: bring-up + medir luz.
 * Encapsula esp_video/esp_cam_sensor. El resto del firmware no conoce el pipeline CSI.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* true si camera_init() termino con exito y la camara esta util. El modo
 * ausente lo comprueba antes de armarse: sin camara no hay vigilancia, y
 * camera_init puede fallar sin que el resto del firmware se entere. */
bool camera_ready(void);

/* true si la camara esta devolviendo imagen corrupta (ruido). Se recupera por
 * software (reset del sensor por I2C); si tras varios intentos sigue igual, es
 * que el sensor no acepta ordenes y hay que cortar la corriente. */
bool camera_corrupta(void);
int  camera_grano(void);

/* true si la imagen sale ROJA saturada porque el AWB del ISP se ha ido (pasa tras
 * recuperar el sensor). Se arregla reiniciando la placa, que es lo unico que
 * rearma el ISP; el propio modulo lo hace solo (con tope de reinicios). */
bool camera_roja(void);
int  camera_rg(void);          /* R/G x100 de la ultima miniatura (diagnostico) */

/* Reintenta el arranque de la camara si no llego a arrancar (sensor que no
 * contesta al encender). Devuelve true si la camara esta util. Se llama desde
 * donde se usa (snapshot, modo vigilancia); como mucho lo intenta cada 3 s. */
bool camera_reintentar(void);

/* Luminosidad ambiente media del ultimo frame (0-255, suavizada). Devuelve
 * false si aun no hay frame valido. Base del auto-brillo. */
bool camera_get_luma(uint8_t *out_luma);

/* Crea el cerrojo del bus camara<->SD. Llamar UNA VEZ desde app_main, ANTES de
 * arrancar el portal y cualquier cosa que toque la tarjeta (16-oct-2026).
 *
 * Sin esto el mutex lo creaba camera_init(): si la camara no arrancaba, no
 * existia, y camera_sd_bus_lock() devolvia true SIN TOMAR NADA -- el cerrojo se
 * corrompia en cuanto la camara se recuperaba en un reintento. Es idempotente:
 * llamarlo dos veces no hace nada. */
void camera_sd_bus_init(void);

/* Cerrojo de bus camara<->SD: los escritores de SD (datalogger/battery_history/log)
 * DEBEN envolver su I/O con estas para no solapar con el DMA de la camara (la
 * contencion en el controlador SDMMC provoca INT WDT -> reinicio). lock devuelve
 * false si no consigue el bus en timeout_ms (entonces omitir la escritura y reintentar
 * luego). Si el cerrojo no esta creado (arranque) permite siempre.
 *
 * unlock COMPRUEBA QUE LO TIENE QUIEN LO SUELTA (16-oct-2026): soltar un mutex
 * que no se posee es un configASSERT (panic) en FreeRTOS, y ademas cuenta la
 * profundidad, asi que un lock/unlock anidado del mismo dueno no suelta el bus
 * antes de tiempo. */
bool camera_sd_bus_lock(uint32_t timeout_ms);
void camera_sd_bus_unlock(void);

/* Para un cierre (fclose/closedir/unlink...) que TIENE que ocurrir de todas
 * formas -- no es una escritura que se pueda simplemente omitir y reintentar
 * luego, dejarlo sin hacer seria una fuga de fd/handle. El idiom que se
 * repetia a mano por todo el portal, "while (!camera_sd_bus_lock(1000))
 * vTaskDelay(1);", esperaba SIN LIMITE: en el contexto de un handler httpd
 * (pocos workers) un bus SD atascado de verdad podia dejar ese hilo colgado
 * para siempre, asfixiando el portal entero. Reintenta hasta total_timeout_ms
 * y devuelve false sin el cerrojo si se agota -- el llamador hace el cierre
 * igual (mejor sin cerrojo que fuga de fd), solo que avisando. Detectado
 * auditando el 08-sep-2026. */
bool camera_sd_bus_lock_wait(uint32_t total_timeout_ms);

/* ── LECTORES DE SD QUE SUELTAN EL CERROJO A MITAD (16-oct-2026) ─────────────
 *
 * Los que leen un fichero GRANDE (el .tar de un viaje, un JPEG de vigilancia)
 * sueltan el cerrojo del bus entre trozos para que la camara recupere su ventana
 * de GDMA: si no, el bus queda tomado durante todo el envio, que por Wi-Fi puede
 * durar minutos. Lo que no se puede es DESMONTAR la tarjeta en medio de uno de
 * esos huecos: el lector se encuentra el descriptor cerrado y da el fichero por
 * acabado (lectura corta), asi que el usuario se baja un .tar truncado o ve una
 * foto a medias y el unico rastro es una linea en el log.
 *
 * Estas dos funciones son ese aviso: el lector las llama al abrir el fichero y
 * al cerrarlo, y datalogger_close_sd() ("Soltar tarjeta") se niega a desmontar
 * mientras haya alguno abierto. Son de CONTAR y ya: no toman ningun cerrojo (el
 * cerrojo del bus lo sigue tomando el lector, como hasta ahora).
 *
 * REGLA: cada sd_reader_abrir() tiene que tener su sd_reader_cerrar(), pase lo
 * que pase (tambien en los caminos de error), o "Soltar tarjeta" dejara de
 * funcionar hasta el siguiente reinicio. */
void sd_reader_abrir(void);
void sd_reader_cerrar(void);
int  sd_reader_cuantos(void);

/* Handle de la tarea de streaming (NULL si la camara no arranco). Para que
 * operaciones que se sabe que bloquean mucho rato (p.ej. el borrado/escritura
 * de flash de una OTA) puedan desuscribirla temporalmente del Task Watchdog
 * de ESP-IDF con esp_task_wdt_delete()/esp_task_wdt_add(), evitando un reset
 * espureo mientras dura. */
TaskHandle_t camera_stream_task_handle(void);

/* Codifica el ultimo frame a JPEG por HW (recorte 960x528: el valor real esta en
 * JPEG_H, camera.c -- el 544 de antes era del thumbnail). THREAD-SAFE (mutex del
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

/* Dice si alguien necesita la LUMINOSIDAD del ambiente (el auto-brillo). Con
 * esto en false y sin vigilancia, la camara deja de capturar: se ahorra el
 * fotograma cada 2 s y el bus queda libre para la SD. */
void camera_set_luma_wanted(bool on);

/* Anillo de capturas de vigilancia en RAM (servido por el HTTP /vigilancia). */
#include <time.h>
/* Lista las capturas (mas nueva primero). Rellena ids/ts/lens hasta max -> count. */
int  camera_vig_list(uint32_t *ids, time_t *ts, size_t *lens, int max);
/* Copia el JPEG de la captura 'id' a un buffer nuevo (caller hace free). false si rotada. */
bool camera_vig_fetch(uint32_t id, uint8_t **out, size_t *out_len);

/* Estado de la sesion de vigilancia en curso, para poder contarlo por HTTP
 * (/ausente) y para que la limpieza por espacio libre no borre lo que se esta
 * grabando. */
int  camera_vig_sesion_fotos(void);           /* fotos que lleva en la tarjeta (0 si no hay sesion) */
bool camera_vig_rotando(void);                /* ya llego al tope y borra las mas antiguas */
bool camera_vig_sesion_actual(char *out, size_t n);  /* "AAAAMMDD_HHMMSS", false si no hay */

#ifdef __cplusplus
}
#endif
