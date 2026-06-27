#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Captura el framebuffer actual del display y lo guarda como BMP de 24 bits
 * (abrible en cualquier PC) en 'path'. Toma el lock de LVGL internamente para
 * leer el framebuffer de forma coherente. Devuelve ESP_OK si lo guardo. */
esp_err_t screenshot_save_bmp(const char *path);

#ifdef __cplusplus
}
#endif
