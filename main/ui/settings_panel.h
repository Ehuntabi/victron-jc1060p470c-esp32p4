#ifndef UI_SETTINGS_PANEL_H
#define UI_SETTINGS_PANEL_H

#include <stdint.h>
#include "ui_state.h"

void ui_settings_panel_init(ui_state_t *ui,
                            const char *default_ssid,
                            const char *default_pass,
                            uint8_t ap_enabled);

void ui_settings_panel_on_user_activity(ui_state_t *ui);
void ui_settings_panel_set_mac(ui_state_t *ui, const char *mac_str);

// Update device status for a specific MAC address in Victron Keys page
void ui_settings_panel_update_victron_device_status(ui_state_t *ui, const char *mac_address, 
                                                     const char *device_type, const char *product_name, 
                                                     const char *error_info);

// Refresh Victron device configuration list (call after devices are added/removed)
void ui_settings_panel_refresh_victron_devices(ui_state_t *ui);

// Force view update when display mode changes
void ui_force_view_update(void);

/* Vuelve a la página principal del menu de Settings (la que muestra las
 * entradas Display/Wi-Fi/Frigo/etc). Util al cerrar Settings desde el boton
 * Home para que la proxima entrada empiece siempre arriba del todo. */
void ui_settings_panel_go_to_main(void);

/* Navegacion programatica de sub-paginas de Ajustes (para el tour de
 * capturas). page_count devuelve cuantas hay; show_page(idx) navega a la
 * pagina idx y la popula la primera vez. Orden = orden de creacion:
 * 0=Frigo 1=Logs 2=Wi-Fi 3=Display 4=Sonido 5=Victron Keys 6=About. */
int  ui_settings_panel_page_count(void);
void ui_settings_panel_show_page(int idx);

/* Mostrar el diálogo modal "Cambio en Wi-Fi — requiere reiniciar".
 * El caller ya debe haber guardado el nuevo estado en NVS antes de llamar.
 * El botón Cancelar revierte el NVS al opuesto y sincroniza el checkbox
 * de Settings (ui->wifi.ap_enable) si está creado. */
void ui_show_wifi_restart_dialog(ui_state_t *ui);

#endif /* UI_SETTINGS_PANEL_H */
void ui_settings_screensaver_create_timer(ui_state_t *ui);
