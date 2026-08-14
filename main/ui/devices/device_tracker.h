/* device_tracker.h - seguimiento de actividad/offline de dispositivos Victron
 * en la pagina Victron Keys de Settings. Extraido de ui.c (god-file) el
 * 2026-08-13: antes vivia mezclado con tabview/barra/screensaver. */
#pragma once

#include <stddef.h>
#include "ui/widgets/ui_state.h"
#include "victron_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Crea el timer que marca "Offline" a los dispositivos sin actividad
 * reciente (cada 10s, timeout 30s). Llamar una vez desde ui_init(). */
void device_tracker_init(ui_state_t *ui);

/* Refresca el timestamp de "visto por ultima vez" de mac_address. */
void ui_update_device_activity(ui_state_t *ui, const char *mac_address);

/* Compone la linea de estado detallado (SOC/tension/potencia segun el tipo)
 * que se muestra en Victron Keys para el ultimo dato recibido. */
void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size);

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
