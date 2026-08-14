/* Pagina "Wi-Fi" de Ajustes: modo punto de acceso, SSID, contrasena y el aviso
 * de reinicio para aplicar cambios.
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver settings_common.h).
 * Es el mismo codigo movido de sitio, sin cambios de comportamiento.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "ui.h"
#include "ui/widgets/ui_card.h"
#include "ui/vigilancia/ausente_mode.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "alerts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "config_storage.h"
#include "portal/config_server.h"
#include "victron_ble.h"
#include "display.h"
#include "esp_log.h"
#include "datalogger.h"
#include "battery_history.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ui/views/frigo_panel.h"
#include "ne185/ne185.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "camera.h"   /* camera_sd_bus_lock: coordinar SD con el GDMA de la camara */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "watchdog.h"
#include "config_backup.h"
#include "data/trip_computer.h"
#include <time.h>
#include "settings_common.h"

void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled);
/* Callbacks propios de esta pagina, definidos mas abajo. */
static void portal_page_cb(lv_event_t *e);
static void reactivate_portal_cb(lv_event_t *e);
static void ap_switch_cb(lv_event_t *e);

/* Namespace NVS donde vive la configuracion Wi-Fi. */
#define WIFI_NAMESPACE "wifi"
void wifi_event_cb(lv_event_t *e);
void password_toggle_btn_event_cb(lv_event_t *e);

static void ap_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = lv_event_get_target(e);
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    (void)ui;
    /* Guardar el nuevo estado en NVS y aplicarlo EN CALIENTE: antes esto solo
     * escribia NVS y sacaba un modal "hay que reiniciar". wifi_ap_init() ya
     * estaba escrita para re-invocarse (flags de init separados, netif creado
     * una sola vez, handlers idempotentes), asi que lo unico que faltaba era
     * llamarla — y hacerlo FUERA del hilo de LVGL, que es lo que hace
     * config_server_request_wifi_apply(). */
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", checked ? 1 : 0);
        esp_err_t err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "wifi enabled (settings) no persistio: %s", esp_err_to_name(err));
        nvs_close(h);
    }

    ui_wifi_set_enabled_cache(checked);   /* el icono de la barra cachea el flag */
    config_server_request_wifi_apply();
}

void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled)
{
    (void)ap_enabled;
    style_settings_scrollbar(page_wifi);
    /* Root container: ROW_WRAP + SPACE_BETWEEN para que las 2 primeras
     * cards (Punto de acceso + Pagina inicial portal) queden lado a lado
     * a pct(49), y la tercera (Reactivar portal web) ocupe linea entera
     * a pct(100) -- mismo patron que frigo_panel. */
    lv_obj_t *cont = lv_obj_create(page_wifi);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Punto de acceso (mitad ancho, lado izdo) === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(49));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_radius(card1, 12, 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_set_style_pad_gap(card1, 12, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Header row: titulo + switch on/off */
    lv_obj_t *card1_header = lv_obj_create(card1);
    lv_obj_remove_style_all(card1_header);
    lv_obj_set_size(card1_header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card1_header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card1_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card1_title = lv_label_create(card1_header);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_WIFI "  Punto de acceso");

    /* Switch ON/OFF */
    lv_obj_t *sw_ap = lv_switch_create(card1_header);
    lv_obj_set_style_bg_color(sw_ap, lv_color_hex(0x4FC3F7), LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ap_enabled) lv_obj_add_state(sw_ap, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_ap, ap_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    ui->wifi.ap_enable = sw_ap;

    /* SSID row: label + input */
    lv_obj_t *ssid_row = lv_obj_create(card1);
    lv_obj_remove_style_all(ssid_row);
    lv_obj_set_width(ssid_row, lv_pct(100));
    lv_obj_set_height(ssid_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(ssid_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_ssid = lv_label_create(ssid_row);
    lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_ssid, "SSID:");

    ui->wifi.ssid = lv_textarea_create(ssid_row);
    lv_obj_set_style_text_font(ui->wifi.ssid, &lv_font_montserrat_24_es, 0);
    lv_textarea_set_one_line(ui->wifi.ssid, true);
    lv_obj_set_width(ui->wifi.ssid, 350);
    lv_textarea_set_text(ui->wifi.ssid, default_ssid);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_READY, ui);
    /* Persistir en NVS solo al confirmar (READY/DEFOCUSED), no por cada tecla
     * (evita un nvs_commit por pulsacion -> desgaste de flash). */
    lv_obj_add_event_cb(ui->wifi.ssid, wifi_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, wifi_event_cb, LV_EVENT_DEFOCUSED, ui);

    /* Password row */
    lv_obj_t *pass_row = lv_obj_create(card1);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_width(pass_row, lv_pct(100));
    lv_obj_set_height(pass_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(pass_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pass_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(pass_row, 8, 0);
    lv_obj_set_flex_align(pass_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_pass = lv_label_create(pass_row);
    lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_pass, "Password:");

    /* Lo que haya guardado; en blanco si aun no se ha generado (wifi_ap_init la
     * crea aleatoria al arrancar). Antes caia a DEFAULT_AP_PASSWORD y ensenaba
     * "12345678" aunque el AP usara otra cosa. 2026-07-26. */
    const char *ap_password = (default_pass && default_pass[0] != '\0') ? default_pass : "";

    /* Boton ojito a la IZQUIERDA del textarea */
    ui->wifi.password_toggle = lv_btn_create(pass_row);
    lv_obj_set_size(ui->wifi.password_toggle, 50, 40);
    lv_obj_add_event_cb(ui->wifi.password_toggle, password_toggle_btn_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_toggle = lv_label_create(ui->wifi.password_toggle);
    lv_label_set_text(lbl_toggle, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(lbl_toggle);

    ui->wifi.password = lv_textarea_create(pass_row);
    lv_obj_set_style_text_font(ui->wifi.password, &lv_font_montserrat_24_es, 0);
    lv_textarea_set_password_mode(ui->wifi.password, true);
    lv_textarea_set_one_line(ui->wifi.password, true);
    lv_obj_set_width(ui->wifi.password, 280);
    lv_textarea_set_text(ui->wifi.password, ap_password);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_READY, ui);
    /* Persistir en NVS solo al confirmar (READY/DEFOCUSED), no por cada tecla
     * (evita un nvs_commit por pulsacion -> desgaste de flash). */
    lv_obj_add_event_cb(ui->wifi.password, wifi_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(ui->wifi.password, wifi_event_cb, LV_EVENT_DEFOCUSED, ui);

    /* === Card 2: Pagina inicial del portal + Reactivar (mitad ancho, dcho) ===
     * El desplegable de pagina inicial y, JUSTO DEBAJO, el boton para
     * reactivar el portal web (antes era una card independiente a lo ancho). */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(49));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 24, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);
    /* Reparte las dos filas en vertical: desplegable arriba, Reactivar abajo,
     * de modo que llenen la card (misma altura que "Punto de acceso"). */
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Fila 1: titulo + desplegable de pagina inicial */
    lv_obj_t *card2_row1 = lv_obj_create(card2);
    lv_obj_remove_style_all(card2_row1);
    lv_obj_set_width(card2_row1, lv_pct(100));
    lv_obj_set_height(card2_row1, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row1, LV_LAYOUT_FLEX);
    /* COLUMN: el desplegable baja a la linea de debajo del titulo para que no
     * se corte en la card estrecha (pct 49). */
    lv_obj_set_flex_flow(card2_row1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card2_row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card2_row1, 10, 0);

    lv_obj_t *card2_title = lv_label_create(card2_row1);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_LIST "  Pagina inicial portal");

    /* Dropdown: 0=Keys, 1=Logs, 2=Dashboard */
    lv_obj_t *dd_portal = lv_dropdown_create(card2_row1);
    lv_obj_set_width(dd_portal, lv_pct(100));
    lv_dropdown_set_options(dd_portal, "Keys\nLogs\nDashboard");
    {
        nvs_handle_t h;
        uint8_t v = 2; /* default: Dashboard */
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "portal_page", &v);
            nvs_close(h);
        }
        if (v > 2) v = 2;
        lv_dropdown_set_selected(dd_portal, v);
    }
    lv_obj_add_event_cb(dd_portal, portal_page_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Fila 2: Portal web + boton Reactivar, justo debajo del desplegable.
     * El servidor HTTP se apaga solo tras 15 min sin nuevas asociaciones
     * (auto-off por seguridad). Este boton lo arranca de nuevo sin tener
     * que reasociar el movil ni reiniciar el display. */
    lv_obj_t *card2_row2 = lv_obj_create(card2);
    lv_obj_remove_style_all(card2_row2);
    lv_obj_set_width(card2_row2, lv_pct(100));
    lv_obj_set_height(card2_row2, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_react_title = lv_label_create(card2_row2);
    lv_obj_set_style_text_font(card2_react_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_react_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(card2_react_title, LV_SYMBOL_REFRESH "  Portal web");

    lv_obj_t *btn_react = lv_btn_create(card2_row2);
    lv_obj_set_height(btn_react, 44);
    lv_obj_set_style_pad_hor(btn_react, 16, 0);
    lv_obj_set_style_radius(btn_react, 8, 0);
    lv_obj_set_style_bg_color(btn_react, lv_color_hex(0x4FC3F7), 0);
    lv_obj_t *btn_lbl = lv_label_create(btn_react);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0x0A0A0A), 0);
    lv_label_set_text(btn_lbl, "Reactivar");
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn_react, reactivate_portal_cb, LV_EVENT_CLICKED, NULL);

    /* ── Card 4: credenciales del portal web (solo lectura) ──────────────
     * La pass HTTP es aleatoria (no derivable de la MAC). Se muestra aqui
     * para que el dueno pueda entrar a la web; solo visible en la pantalla
     * fisica del display. */
    char web_user[33] = {0};
    char web_pass[33] = {0};
    config_server_get_web_credentials(web_user, sizeof(web_user),
                                      web_pass, sizeof(web_pass));

    lv_obj_t *card4 = lv_obj_create(cont);
    lv_obj_set_width(card4, lv_pct(100));
    lv_obj_set_height(card4, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card4, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card4, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card4, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card4, 1, 0);
    lv_obj_set_style_radius(card4, 12, 0);
    lv_obj_set_style_pad_all(card4, 16, 0);
    lv_obj_set_style_pad_gap(card4, 6, 0);
    lv_obj_set_layout(card4, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card4, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *c4_title = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(c4_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(c4_title, LV_SYMBOL_SETTINGS "  Acceso a Actualizar y Claves");

    lv_obj_t *c4_user = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_user, &lv_font_montserrat_20_es, 0);
    lv_label_set_text_fmt(c4_user, "Usuario:  %s",
                          web_user[0] ? web_user : "victron");

    lv_obj_t *c4_pass = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_pass, &lv_font_montserrat_20_es, 0);
    lv_label_set_text_fmt(c4_pass, "Clave:  %s",
                          web_pass[0] ? web_pass : "(sin definir)");

    lv_obj_t *c4_hint = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_hint, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(c4_hint, lv_color_hex(0x888888), 0);
    /* Desde 2026-08-07 hay DOS niveles (ver config_server.c): la web normal no
     * pide nada — basta con estar en el Wi-Fi del display — pero /ota, /keys y
     * /save SI, porque reescriben el firmware o entregan las claves AES. Hay
     * que decir exactamente donde hacen falta: anunciarlas como necesarias para
     * todo fue lo que hizo perder el tiempo buscando por que la app no
     * conectaba, y decir que no hacen falta para nada seria mentir ahora. */
    lv_label_set_text(c4_hint,
                      "Solo para Actualizar (/ota) y Claves Victron. El resto\n"
                      "de la web y la app no piden nada: basta con el Wi-Fi.");

    /* Igualar la altura de la card "Pagina inicial portal" a la de "Punto de
     * acceso" (la mas alta) para que ambas queden simetricas lado a lado. */
    lv_obj_update_layout(cont);
    lv_coord_t h_ap = lv_obj_get_height(card1);
    if (h_ap > lv_obj_get_height(card2)) {
        lv_obj_set_height(card2, h_ap);
    }
}

static void reactivate_portal_cb(lv_event_t *e)
{
    (void)e;
    /* Encolar, NO llamar a config_server_start() aqui: monta SPIFFS y lee NVS,
     * y estamos en el hilo de LVGL. El arranque es idempotente, asi que si el
     * portal ya estaba arriba el trabajo no hace nada. */
    config_server_request_start();
    ESP_LOGI("settings_panel", "Reactivar portal web: solicitado");
}

void wifi_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    const char *txt = lv_textarea_get_text(ta);
    if (ui == NULL) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (ta == ui->wifi.ssid) {
            err = nvs_set_str(h, "ssid", txt);
        } else if (ta == ui->wifi.password) {
            err = nvs_set_str(h, "password", txt);
        }
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "Wi-Fi ssid/password (set) no persistio: %s", esp_err_to_name(err));
        err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "Wi-Fi config (commit) no persistio: %s", esp_err_to_name(err));
        nvs_close(h);
        ESP_LOGI(TAG_SETTINGS, "Wi-Fi config saved");
    } else {
        ESP_LOGE(TAG_SETTINGS, "nvs_open failed: %s", esp_err_to_name(err));
    }
}

void password_toggle_btn_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->wifi.password == NULL) {
        return;
    }

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    bool new_mode = !lv_textarea_get_password_mode(ui->wifi.password);
    lv_textarea_set_password_mode(ui->wifi.password, new_mode);

    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_label_set_text(label, new_mode ? "Show" : "Hide");
    }
}

static void portal_page_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "portal_page", (uint8_t)sel);
        esp_err_t err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "portal_page no persistio: %s", esp_err_to_name(err));
        nvs_close(h);
    }
    const char *name = sel == 0 ? "Keys" : (sel == 1 ? "Logs" : "Dashboard");
    ESP_LOGI(TAG_SETTINGS, "Portal page: %s", name);
}
