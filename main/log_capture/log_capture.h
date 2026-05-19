#pragma once

/* Captura en RAM de todos los ESP_LOGx via esp_log_set_vprintf.
 * Buffer circular en PSRAM (~120 KB).
 * Los logs siguen saliendo por UART normalmente (chain al vprintf previo).
 * Thread-safe (mutex). */

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_CAPTURE_MAX_LINES 500
#define LOG_CAPTURE_MAX_TAG   24
#define LOG_CAPTURE_MAX_MSG   200

typedef struct {
    uint32_t seq;                       /* numero secuencial estrictamente creciente */
    uint32_t ts_ms;                     /* timestamp del prefijo IDF "(12345)" */
    esp_log_level_t level;              /* E/W/I/D/V parseado del prefijo */
    char tag[LOG_CAPTURE_MAX_TAG];      /* tag del log (null-terminated, puede estar vacio) */
    char msg[LOG_CAPTURE_MAX_MSG];      /* mensaje sin prefijo IDF (null-terminated, sin \n) */
} log_capture_entry_t;

/* Instalar hook vprintf. Llamar lo antes posible en app_main para capturar
 * desde el boot. Idempotente: llamadas posteriores se ignoran. */
void log_capture_init(void);

/* Copia las ultimas N entradas (las mas recientes al final) filtradas.
 *   out          buffer destino (caller-allocated)
 *   max          tamano de out (entradas)
 *   level_mask   bitmask de niveles a incluir: bit 0=E, 1=W, 2=I, 3=D, 4=V.
 *                Usar 0 para "todos".
 *   tag_substr   substring case-insensitive a buscar en el tag. NULL/"" para "todos".
 * Returns: numero de entradas escritas en out (<= max). */
size_t log_capture_get_lines(log_capture_entry_t *out, size_t max,
                              uint8_t level_mask, const char *tag_substr);

/* Ultimo numero de secuencia escrito. Para detectar si hay logs nuevos
 * sin tener que copiar todo el buffer. */
uint32_t log_capture_last_seq(void);

/* Vacia el buffer (resetea count, head, seq). */
void log_capture_clear(void);

/* Vuelca el buffer completo (no filtrado) a un archivo de texto.
 * Formato: "12345 [I] tag: msg\n" por linea.
 * path: ruta absoluta (ej. "/sdcard/log_20260520_103045.txt"). */
esp_err_t log_capture_save_to_file(const char *path);

/* Auto-save: genera el path "/sdcard/log_<reason>_<YYYYMMDD_HHMMSS>.txt",
 * donde <reason> proviene de esp_reset_reason() del boot actual (power/wdt/
 * panic/sw/brownout/...), y aplica rotacion FIFO dejando como max `keep`
 * archivos "log_*.txt" en /sdcard. SD debe estar montada antes de llamar. */
esp_err_t log_capture_autosave_now(int keep);

#ifdef __cplusplus
}
#endif
