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
void ui_set_freezer_alarm(ui_state_t *ui, bool active);
void ui_show_chart_screen(ui_state_t *ui);
void ui_show_battery_history_screen(ui_state_t *ui);

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
