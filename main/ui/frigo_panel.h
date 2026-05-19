#pragma once
#include "ui/ui_state.h"
#include "frigo.h"

void ui_frigo_panel_init(ui_state_t *ui);
void ui_frigo_panel_update(ui_state_t *ui, const frigo_state_t *state);

/* Cierra los dropdowns abiertos del panel Frigo (si los hay). Necesario al
 * salir de Settings (auto-return tras 60 s o pulsar el icono Home) porque
 * la lista flotante del dropdown LVGL no se cierra sola y se ve flotando
 * sobre la vista Live. */
void ui_frigo_panel_close_dropdowns(void);
