/* ui.h */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <lvgl.h>
#include "victron_ble.h"
#include "ui/ui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize all LVGL UI elements, including Live, Settings and Frigo tabs.
 */
void ui_init(void);
void ui_update_wifi_ssid(ui_state_t *ui);

/**
 * BLE data callback to update the UI with new panel data.
 * @param d Pointer to the victron_data_t structure containing sensor readings.
 */
void ui_on_panel_data(const victron_data_t *d);
void ui_set_ble_mac(const uint8_t *mac);

/* Notify the UI that the user performed an activity (e.g. touch).
 * This will reset the screensaver timer and restore brightness if active.
 */
void ui_notify_user_activity(void);

/* Devuelve true si el salvapantallas esta activo (atenuado o rotando).
 * Permite que el primer toque solo lo despierte y no se propague como
 * click/gesto al widget de debajo. */
bool ui_screensaver_is_active(void);

/* Forzar un refresco inmediato del label de la hora.
 * Útil tras inicializar el RTC y configurar la hora del sistema en arranque. */
void ui_refresh_clock(void);
void ui_set_freezer_alarm(ui_state_t *ui, bool active);
/* Estado actual de la alarma del congelador (criterio robusto de main.c).
 * La vista Overview lo consulta en vez de re-evaluar el umbral. */
bool ui_get_freezer_alarm(void);
void ui_show_chart_screen(ui_state_t *ui);
void ui_show_battery_history_screen(ui_state_t *ui);

/* true si hay alguna alarma activa (no silenciada) en la vista Overview
 * (S1 agua/ R1 agua / SoC / congelador). La consulta el salvapantallas para
 * no rotar mientras hay alarma. */
bool ui_overview_alarm_active(void);

/* Si el salvapantallas esta rotando, lo interrumpe y salta a Live + Overview
 * para que la alarma sea visible. Llamado desde la deteccion de alarmas. */
void ui_alarm_interrupt_screensaver(void);

/**
 * Mark a device as offline in the Victron Keys settings page.
 * @param mac_address MAC address of the device to mark as offline
 */
void ui_mark_device_offline(const char *mac_address);

/**
 * Refresh the Victron device configuration list in the settings page.
 * Call this after devices are added, removed, or configuration changes.
 */
void ui_refresh_victron_device_list(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
ui_state_t *ui_get_state(void);
void ui_close_chart_screen(void);
void ui_close_battery_history_screen(void);
