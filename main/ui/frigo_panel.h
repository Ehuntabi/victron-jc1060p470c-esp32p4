#pragma once
#include "ui/ui_state.h"
#include "frigo.h"

void ui_frigo_panel_init(ui_state_t *ui);
void ui_frigo_panel_update(ui_state_t *ui, const frigo_state_t *state);
