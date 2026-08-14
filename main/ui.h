/* ui.h */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <lvgl.h>
#include "victron_ble.h"
#include "audio_es8311.h"
#include "ui/widgets/ui_state.h"
#include "ui/devices/device_tracker.h"

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

// Avisa al icono Wi-Fi de la barra del nuevo estado de 'wifi/enabled'. Hay que
// llamarla SIEMPRE que se cambie ese flag: el icono cachea el valor en RAM a
// proposito (leer NVS en cada tick provocaba INT WDT) y no se entera solo.
void ui_wifi_set_enabled_cache(bool enabled);

/* Forzar un refresco inmediato del label de la hora.
 * Útil tras inicializar el RTC y configurar la hora del sistema en arranque. */
void ui_refresh_clock(void);

/* Overlay fijo a pantalla completa durante la actualizacion de firmware por
 * Wi-Fi (ver main/portal/ota_update.c): tapa el parpadeo/tearing normal del
 * buffer unico de LVGL bajo carga de flash (no lo arregla, solo lo oculta) y
 * bloquea el touch para que no se pueda tocar nada mientras la OTA esta en
 * marcha. Toman el lock de LVGL internamente: seguras de llamar desde
 * cualquier tarea (incluida la del httpd, que es de donde se llaman). */
void ui_ota_overlay_show(const char *msg);
void ui_ota_overlay_hide(void);

void ui_set_freezer_alarm(ui_state_t *ui, bool active);
/* Estado actual de la alarma del congelador (criterio robusto de main.c).
 * La vista Overview lo consulta en vez de re-evaluar el umbral. */
bool ui_get_freezer_alarm(void);
void ui_show_chart_screen(ui_state_t *ui);
void ui_show_battery_history_screen(ui_state_t *ui);

/* Abre a pantalla completa la vista de detalle de una card del Overview
 * (Solar / Bateria / DC-DC). `category` es el tipo de record; para DC-DC se
 * resuelve al Orion concreto (Tr 0x04 u XS 0x0F) segun el ultimo dato recibido.
 * Se cierra con el boton volver, con ui_close_card_detail() o solo tras 1 min
 * sin interaccion (reusa el timer idle-to-live). */
void ui_show_card_detail(ui_state_t *ui, victron_record_type_t category);
void ui_close_card_detail(void);

/* true si hay alguna alarma activa (no silenciada) en la vista Overview
 * (S1 agua/ R1 agua / SoC / congelador). La consulta el salvapantallas para
 * no rotar mientras hay alarma. */
bool ui_overview_alarm_active(void);

/* Si el salvapantallas esta rotando, lo interrumpe y salta a Live + Overview
 * para que la alarma sea visible. Llamado desde la deteccion de alarmas. */
void ui_alarm_interrupt_screensaver(void);

/* ui_mark_device_offline / ui_refresh_victron_device_list: ver
 * ui/device_tracker.h (incluido arriba). */

/* Uso interno del modulo UI (ui.c y ui/capture_carousel.c): cambia la vista
 * activa segun el modo de seleccion manual o el tipo de dispositivo BLE
 * recibido. No-static para que el carrusel de capturas pueda forzar la
 * navegacion pantalla a pantalla. */
void ensure_device_layout(ui_state_t *ui, victron_record_type_t type);

/* Encola un jingle para que suene en la task de audio (ui_beep_task), NUNCA
 * en el sitio del llamante (que puede tener el lock LVGL cogido y/o correr
 * en la task NimBLE). priority=true (alarmas) reemplaza un beep pendiente;
 * priority=false (clicks) se descarta si la cola esta llena. No-static para
 * que ui/ble_ingest.c pueda disparar las alarmas de SoC. */
void ui_enqueue_jingle(audio_jingle_t j, bool priority);

ui_state_t *ui_get_state(void);
void ui_close_chart_screen(void);
void ui_close_battery_history_screen(void);
void ui_show_solar_history_screen(ui_state_t *ui);
void ui_close_solar_history_screen(void);

/* Capturas por WiFi: navega a la pantalla idx (0..count-1) y devuelve su nombre
 * corto, o NULL si idx esta fuera de rango. Ver ui.c / config_server.c. */
const char *ui_tour_goto_screen(int idx);
int ui_tour_screen_count(void);

/* Carrusel de captura a demanda: recorre las 8 pantallas de datos (6 vistas +
 * 2 graficos), guarda un BMP de cada una en la SD (sobrescribe) y termina. Lo
 * dispara el switch de Settings->Display; el switch se apaga solo al acabar. */
void ui_start_capture_carousel(void);
bool ui_capture_carousel_running(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
