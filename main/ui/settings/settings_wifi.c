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
static void wifi_pintar_ip_ap(ui_state_t *ui);

/* Namespace NVS donde vive la configuracion Wi-Fi. */
#define WIFI_NAMESPACE "wifi"
static void wifi_save_cb(lv_event_t *e);
static lv_obj_t *s_wifi_estado = NULL;   /* respuesta del boton Guardar */
void password_toggle_btn_event_cb(lv_event_t *e);

/* Pinta la IP del AP donde toca, o "--" si el AP esta apagado. */
static void wifi_pintar_ip_ap(ui_state_t *ui)
{
    if (!ui || !ui->wifi.ap_ip) return;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        lv_label_set_text_fmt(ui->wifi.ap_ip, "IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        lv_label_set_text(ui->wifi.ap_ip, "IP: -- (AP apagado)");
    }
}

static void ap_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = lv_event_get_target(e);
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

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
    wifi_pintar_ip_ap(ui);   /* encender/apagar cambia la IP del AP (o la deja en --) */
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
    lv_obj_set_style_pad_all(cont, 8, 0);    /* compactado 22-sep-2026: */
    lv_obj_set_style_pad_gap(cont, 8, 0);   /* la pagina tiene que caber */

    /* === Card 1: Punto de acceso (mitad ancho, lado izdo) === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(49));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, UI_COLOR_CYAN, 0);
    lv_obj_set_style_border_width(card1, 2, 0);
    lv_obj_set_style_radius(card1, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card1, 8, 0);  /* en 600 px sin deslizar  */
    lv_obj_set_style_pad_gap(card1, 8, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Interruptor + cabecera centrada: el switch va en la cabecera, equilibrado
     * por un espaciador (la tarjeta no crece). */
    lv_obj_t *sw_ap = lv_switch_create(card1);
    lv_obj_set_style_bg_color(sw_ap, UI_COLOR_CYAN, LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ap_enabled) lv_obj_add_state(sw_ap, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_ap, ap_switch_cb, LV_EVENT_VALUE_CHANGED, ui);

    lv_obj_t *card1_title = lv_label_create(card1);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card1_title, UI_COLOR_CYAN, 0);
    lv_label_set_text(card1_title, LV_SYMBOL_WIFI "  Punto de acceso");
    ui_card_wrap_title_with(card1, card1_title, UI_COLOR_CYAN, sw_ap);

    /* Switch ON/OFF */
    ui->wifi.ap_enable = sw_ap;

    /* IP del punto de acceso: es la direccion a la que hay que entrar desde el
     * movil para el portal, asi que va AQUI, con el AP, y no en Acerca de
     * (sugerencia del usuario, 22-sep-2026). Se actualiza al crear la tarjeta y
     * cada vez que se enciende o apaga el AP. */
    ui->wifi.ap_ip = lv_label_create(card1);
    lv_obj_set_style_text_font(ui->wifi.ap_ip, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->wifi.ap_ip, lv_color_hex(0xB0BEC5), 0);
    wifi_pintar_ip_ap(ui);   /* que se vea ya al entrar, no solo al tocar el interruptor */

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
    /* Altura fija y contenida: por defecto el campo venia alto y era lo que
     * estiraba la fila de arriba (idea del usuario, 22-sep-2026). */
    lv_obj_set_height(ui->wifi.ssid, 42);
    lv_textarea_set_one_line(ui->wifi.ssid, true);
    lv_obj_set_width(ui->wifi.ssid, 350);
    /* Tope 802.11: SSID max 32 caracteres. Sin esto se podia teclear un SSID
     * mas largo, que guardar_wifi_cb() (mas abajo) no rechazaba -- solo
     * validaba el minimo de la clave -- y wifi_ap_init() lo lee de NVS sin
     * revalidar. Detectado por el usuario el 09-sep-2026. */
    lv_textarea_set_max_length(ui->wifi.ssid, 32);
    lv_textarea_set_text(ui->wifi.ssid, default_ssid);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_READY, ui);
    /* SIN guardado automatico: lo escribe el boton "Guardar red" de abajo.
     * Guardar al perder el foco fue la mitad de un accidente serio (21-ago-2026):
     * la otra mitad rellenaba esta casilla con un nombre inventado a partir de
     * la MAC, y bastaba tocarla y salir para RENOMBRAR EL AP sin querer,
     * dejando al satelite sin red que buscar. Con un boton, no se guarda nada
     * hasta que se pide. */

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
    lv_obj_set_height(ui->wifi.password, 42);
    lv_obj_set_style_text_font(ui->wifi.password, &lv_font_montserrat_24_es, 0);
    lv_textarea_set_password_mode(ui->wifi.password, true);
    lv_textarea_set_one_line(ui->wifi.password, true);
    lv_obj_set_width(ui->wifi.password, 280);
    /* Tope WPA2-PSK: clave max 63 caracteres. Mismo motivo que el del SSID
     * de arriba. */
    lv_textarea_set_max_length(ui->wifi.password, 63);
    lv_textarea_set_text(ui->wifi.password, ap_password);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_READY, ui);
    /* SIN guardado automatico, igual que el SSID: manda el boton de abajo. */

    /* Boton de guardar, a lo ancho de la card y debajo de los dos campos.
     * Escribe los DOS y reaplica el AP en caliente
     * (config_server_request_wifi_apply), asi el nombre nuevo sale sin
     * reiniciar ni tocar el interruptor del punto de acceso. */
    lv_obj_t *btn_save = lv_btn_create(card1);
    lv_obj_set_width(btn_save, lv_pct(100));
    lv_obj_set_height(btn_save, 38);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2E7D32), 0);
    lv_obj_add_event_cb(btn_save, wifi_save_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_save, "Guardar red");
    lv_obj_center(lbl_save);

    /* Respuesta del boton. Sin esto lo pulsabas, se guardaba, se reiniciaba el
     * AP... y en pantalla no pasaba nada visible: la unica constancia quedaba en
     * el log, que el usuario no ve. */
    s_wifi_estado = lv_label_create(card1);
    lv_label_set_text(s_wifi_estado, "");
    lv_obj_set_style_text_font(s_wifi_estado, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_wifi_estado, lv_color_hex(0x4CD964), 0);
    lv_obj_set_width(s_wifi_estado, lv_pct(100));
    lv_label_set_long_mode(s_wifi_estado, LV_LABEL_LONG_WRAP);


    /* === Card 2: Pagina inicial del portal + Reactivar (mitad ancho, dcho) ===
     * El desplegable de pagina inicial y, JUSTO DEBAJO, el boton para
     * reactivar el portal web (antes era una card independiente a lo ancho). */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));  /* el portal, a lo ancho, debajo */
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, UI_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(card2, 2, 0);
    lv_obj_set_style_radius(card2, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card2, 10, 0);
    lv_obj_set_style_pad_gap(card2, 8, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    /* DOS COLUMNAS (22-sep-2026, idea del usuario): el desplegable de la pagina
     * inicial a la izquierda y el portal web con su boton a la derecha. Ademas de
     * gustarle mas, resuelve el problema de raiz: al ir lado a lado no compiten por
     * el alto y no pueden pisarse (era lo que se veia como "DashPortalweb"). */
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    /* Reparte las dos filas en vertical: desplegable arriba, Reactivar abajo,
     * de modo que llenen la card (misma altura que "Punto de acceso"). */
    /* START + separacion fija en vez de SPACE_BETWEEN (22-sep-2026). Con
     * SPACE_BETWEEN, si la tarjeta se queda mas corta que su contenido, LVGL
     * apila los hijos unos encima de otros: el sintoma era el desplegable
     * pisando el texto. Con START y un hueco fijo eso no puede pasar. */
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* v3.10: la tarjeta pasa a columna; las dos columnas de siempre van a una
     * fila-cuerpo debajo de la cabecera centrada. */
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *card2_body = lv_obj_create(card2);
    lv_obj_remove_style_all(card2_body);
    lv_obj_set_size(card2_body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *card2_hdr = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_hdr, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_hdr, UI_COLOR_GREEN, 0);
    lv_label_set_text(card2_hdr, LV_SYMBOL_LIST "  Portal web");
    ui_card_wrap_title(card2, card2_hdr, UI_COLOR_GREEN);

    /* Fila 1: desplegable de pagina inicial (el titulo ya esta en la cabecera) */
    lv_obj_t *card2_row1 = lv_obj_create(card2_body);
    lv_obj_remove_style_all(card2_row1);
    lv_obj_set_width(card2_row1, lv_pct(62));   /* izquierda: titulo + desplegable */
    lv_obj_set_height(card2_row1, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row1, LV_LAYOUT_FLEX);
    /* COLUMN: el desplegable baja a la linea de debajo del titulo para que no
     * se corte en la card estrecha (pct 49). */
    lv_obj_set_flex_flow(card2_row1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card2_row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card2_row1, 6, 0);

    /* Rotulo del desplegable (el titulo de la tarjeta ya esta en la cabecera). */
    lv_obj_t *card2_title = lv_label_create(card2_row1);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(card2_title, UI_COLOR_TEXT_SOFT, 0);
    lv_label_set_text(card2_title, "Página inicial del portal");

    /* Dropdown: 0=Keys, 1=Logs, 2=Dashboard */
    lv_obj_t *dd_portal = lv_dropdown_create(card2_row1);
    lv_obj_set_width(dd_portal, lv_pct(75));   /* mas estrecho (lo pidio el usuario) */
    lv_dropdown_set_options(dd_portal, "Keys\nLogs\nDashboard");
    lv_obj_set_style_text_font(dd_portal, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd_portal), &lv_font_montserrat_24_es, 0);
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
    lv_obj_t *card2_row2 = lv_obj_create(card2_body);
    lv_obj_remove_style_all(card2_row2);
    lv_obj_set_width(card2_row2, lv_pct(36));   /* derecha: portal web + Reactivar */
    lv_obj_set_height(card2_row2, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row2, LV_LAYOUT_FLEX);
    /* LADO A LADO, no apilados: la sonda de solapamientos midio que la etiqueta
     * 'Portal web' y el boton 'Reactivar' caian en el mismo rectangulo
     * (etiqueta 734,380-893,408 sobre boton 637,380-989,419). */
    lv_obj_set_flex_flow(card2_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_align(card2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_react_title = lv_label_create(card2_row2);
    lv_obj_set_style_text_font(card2_react_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_react_title, UI_COLOR_CYAN, 0);
    lv_label_set_text(card2_react_title, LV_SYMBOL_REFRESH "  Portal web");

    lv_obj_t *btn_react = lv_btn_create(card2_row2);
    lv_obj_set_height(btn_react, 40);
    lv_obj_set_width(btn_react, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn_react, 10, 0);   /* boton mas recogido (lo pidio el usuario) */
    lv_obj_set_style_radius(btn_react, 8, 0);
    lv_obj_set_style_bg_color(btn_react, UI_COLOR_CYAN, 0);
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
    lv_obj_set_width(card4, lv_pct(49));   /* el ACCESO va arriba, junto al AP */
    lv_obj_set_height(card4, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card4, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card4, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card4, UI_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(card4, 2, 0);
    lv_obj_set_style_radius(card4, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card4, 10, 0);
    lv_obj_set_style_pad_gap(card4, 6, 0);
    lv_obj_set_layout(card4, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card4, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *c4_title = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(c4_title, UI_COLOR_GREEN, 0);
    lv_label_set_text(c4_title, LV_SYMBOL_SETTINGS "  Acceso a Actualizar y Claves");
    ui_card_wrap_title(card4, c4_title, UI_COLOR_GREEN);

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
    lv_obj_set_width(c4_hint, lv_pct(100));
    lv_label_set_long_mode(c4_hint, LV_LABEL_LONG_WRAP);
    /* Ancho completo de la tarjeta y que envuelva sola: antes el texto llevaba
     * los saltos de linea escritos a mano y quedaba una columna estrecha con
     * medio ancho de tarjeta vacio (lo vio el usuario, 22-sep-2026). */
    lv_obj_set_width(c4_hint, lv_pct(100));
    lv_label_set_long_mode(c4_hint, LV_LABEL_LONG_WRAP);
    /* OJO con este texto, que ya ha mentido una vez (auditado el 14-sep-2026).
     *
     * El 2026-08-07 esto eran DOS niveles: la web abierta y solo /ota, /keys y
     * /save cerrados. Aquel texto decia, con razon entonces, que "la app no
     * pide nada: basta con el Wi-Fi".
     *
     * El 21-ago-2026 el portal volvio a exigir credenciales TAMBIEN en el nivel
     * abierto (PORTAL_REQUIRE_BASIC_AUTH=1, config_server_auth.c) y ese cartel
     * se quedo con el texto viejo. Resultado: decia justo lo contrario de lo que
     * hace el firmware, y es la clase de cartel que se cree -- la app de Android
     * iba sin credenciales y el portal le contestaba 401 a todo.
     *
     * Ahora dice la verdad, y si algun dia se vuelve a abrir el nivel abierto
     * (poniendo PORTAL_REQUIRE_BASIC_AUTH a 0) hay que cambiar ESTE texto a la
     * vez: son la misma decision contada en dos sitios. */
    lv_label_set_text(c4_hint,
                      "La web, la app del movil y Actualizar (/ota) piden este usuario y "
                      "clave, y se leen aqui. Ojo: la web va por HTTP sin cifrar, "
                      "protegida solo por la clave Wi-Fi de arriba, no por esta.");

    /* Igualar la altura de la card de ACCESO (la que va al lado) a la de "Punto de
     * acceso" (la mas alta) para que ambas queden simetricas lado a lado. */
    /* INTERCAMBIO pedido por el usuario (22-sep-2026): arriba, junto al punto de
     * acceso, va la tarjeta de ACCESO (usuario y clave, que es lo que hace
     * falta para entrar desde el movil); la pagina inicial del portal queda
     * debajo a ancho completo. Es solo el orden del hijo en el contenedor
     * (flex ROW_WRAP): card1 + card4 arriba, card2 debajo. */
    lv_obj_move_to_index(card4, 1);

    lv_obj_update_layout(cont);
    lv_coord_t h_ap = lv_obj_get_height(card1);
    if (h_ap > lv_obj_get_height(card4)) {
        lv_obj_set_height(card4, h_ap);
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

/* Guarda SSID y password y reaplica el AP en caliente. Sustituye al guardado
 * automatico al perder el foco, que renombraba el AP sin pedirlo. */
static void wifi_save_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui == NULL || ui->wifi.ssid == NULL || ui->wifi.password == NULL) return;

    const char *ssid = lv_textarea_get_text(ui->wifi.ssid);
    const char *pass = lv_textarea_get_text(ui->wifi.password);

    /* Un SSID vacio dejaria el AP sin nombre: wifi_ap_init lo rechaza y vuelve
     * al de fabrica, asi que mejor no guardar nada y que se vea que no ha
     * pasado nada. */
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGW(TAG_SETTINGS, "SSID vacio: no se guarda");
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "El nombre de la red no puede estar vacio");
        }
        return;
    }
    /* Tope 802.11 (32 caracteres). lv_textarea_set_max_length ya lo impide
     * al teclear, pero esto es la validacion de verdad -- por si algun dia
     * el SSID llega por otro camino que no pase por ese textarea. */
    if (strlen(ssid) > 32) {
        ESP_LOGW(TAG_SETTINGS, "SSID de %u caracteres: no se guarda (max 32)",
                 (unsigned)strlen(ssid));
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "El nombre de la red no puede pasar de 32 caracteres");
        }
        return;
    }
    /* WPA2 exige 8-63 caracteres. wifi_ap_init() (config_server_ap.c) lee esta
     * clave de NVS SIN revalidarla -- confia en que config_server_ensure_ap_
     * password() ya la dejo buena, pero esa funcion solo corre UNA VEZ al
     * arrancar, antes de la UI. Sin este chequeo, guardar aqui una clave
     * corta (o en blanco) desde la pantalla la dejaba tal cual en NVS, y el
     * siguiente wifi_ap_init() la usaba igual -> AP con WPA2 mal configurado,
     * probablemente sin arrancar hasta reflashear por USB. Detectado
     * auditando el 08-sep-2026. */
    size_t pass_len = pass ? strlen(pass) : 0;
    if (pass_len < 8) {
        ESP_LOGW(TAG_SETTINGS, "clave WPA2 de %u caracteres: no se guarda", (unsigned)pass_len);
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "La clave Wi-Fi necesita al menos 8 caracteres (WPA2)");
        }
        return;
    }
    /* Tope WPA2-PSK (63 caracteres), mismo motivo que el del SSID de arriba:
     * validacion de verdad, no solo el max_length del textarea. */
    if (pass_len > 63) {
        ESP_LOGW(TAG_SETTINGS, "clave WPA2 de %u caracteres: no se guarda (max 63)", (unsigned)pass_len);
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "La clave Wi-Fi no puede pasar de 63 caracteres (WPA2)");
        }
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SETTINGS, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "password", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "Wi-Fi config no persistio: %s", esp_err_to_name(err));
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "No se pudo guardar");
        }
        return;
    }
    ESP_LOGI(TAG_SETTINGS, "Wi-Fi guardado: SSID='%s' -> reaplicando AP", ssid);
    if (s_wifi_estado) {
        lv_obj_set_style_text_color(s_wifi_estado, lv_color_hex(0x4CD964), 0);
        lv_label_set_text_fmt(s_wifi_estado, "Guardado. Reconectando como \"%s\"...", ssid);
    }
    config_server_request_wifi_apply();
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
