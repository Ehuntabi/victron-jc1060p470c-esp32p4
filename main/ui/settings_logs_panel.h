#pragma once
#include "lvgl.h"
#include "ui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Crea la pagina "Consola" dentro del menu de Settings: textarea grande con
 * los ESP_LOGx capturados en tiempo real, filtros por nivel y por tag,
 * pause/play, limpiar buffer y guardar a SD. */
void settings_logs_panel_create(ui_state_t *ui, lv_obj_t *page);

#ifdef __cplusplus
}
#endif
