#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Captura el framebuffer actual del display y lo guarda como BMP de 24 bits
 * (abrible en cualquier PC) en 'path'. Toma el lock de LVGL internamente para
 * leer el framebuffer de forma coherente. Devuelve ESP_OK si lo guardo. */
esp_err_t screenshot_save_bmp(const char *path);

/* Detalle textual del ultimo fallo de screenshot_save_bmp (errno + paso), para
 * mostrarlo en la UI sin depender del monitor serie. Cadena vacia si no hubo. */
const char *screenshot_last_error(void);

/* Captura la pantalla activa como BMP de 24 bits EN MEMORIA (para servirla por
 * HTTP sin depender de la SD). Deja en *out_buf un buffer PSRAM con el BMP
 * completo (cabecera + pixeles) y su tamano en *out_len. El llamante debe
 * liberarlo con heap_caps_free(). Toma el lock de LVGL internamente. */
esp_err_t screenshot_take_bmp(uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif
