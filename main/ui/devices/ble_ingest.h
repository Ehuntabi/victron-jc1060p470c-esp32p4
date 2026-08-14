/* ble_ingest.h - alimenta battery_history con cada record BLE y detecta el
 * cruce de los umbrales de alarma de SoC (critico/warning).
 * Extraido de ui.c (god-file) el 2026-08-13. */
#pragma once

#include "victron_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Llamar con CADA record BLE recibido (ui_on_panel_data). No toca LVGL ni
 * g_ui: solo battery_history/solar_daily y, si el SoC cruza hacia abajo el
 * umbral critico o warning (alerts_get_soc_*), encola el jingle
 * correspondiente via ui_enqueue_jingle(). */
void ble_ingest_feed_history(const victron_data_t *d);

#ifdef __cplusplus
}
#endif
