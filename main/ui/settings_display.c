/* Pagina "Pantalla" de Ajustes: brillo, modo nocturno, salvapantallas, vista de
 * inicio y pantalla de arranque.
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver settings_common.h).
 * Es el mismo codigo movido de sitio, sin cambios de comportamiento. Sus
 * callbacks no guardan estado propio: todo va en ui_state_t.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "ui.h"
#include "ui_card.h"
#include "ausente_mode.h"
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
#include "config_server.h"
#include "victron_ble.h"
#include "display.h"
#include "esp_log.h"
#include "datalogger.h"
#include "battery_history.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ui/frigo_panel.h"
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
#include "trip_computer.h"
#include <time.h>
#include "settings_common.h"

void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display);
/* Callbacks propios de esta pagina (definidos mas abajo). */
static void brightness_slider_event_cb(lv_event_t *e);
static void cb_screensaver_event_cb(lv_event_t *e);
static void night_end_dec_cb(lv_event_t *e);
static void night_end_inc_cb(lv_event_t *e);
static void night_start_dec_cb(lv_event_t *e);
static void night_start_inc_cb(lv_event_t *e);
static void night_switch_cb(lv_event_t *e);
static void slider_ss_brightness_event_cb(lv_event_t *e);
static void spinbox_ss_time_decrement_event_cb(lv_event_t *e);
static void spinbox_ss_time_increment_event_cb(lv_event_t *e);
static void splash_dropdown_cb(lv_event_t *e);
static void ss_mode_changed_cb(lv_event_t *e);
static void ss_period_dec_cb(lv_event_t *e);
static void ss_period_inc_cb(lv_event_t *e);
static void view_selection_dropdown_event_cb(lv_event_t *e);

/* Definidas en settings_panel.c, pero usadas desde esta pagina: aplican el
 * brillo, el modo nocturno y el salvapantallas, y las comparten varias paginas. */
void apply_brightness_for_now(ui_state_t *ui);
void night_save_and_apply(ui_state_t *ui);
void screensaver_enable(ui_state_t *ui, bool enable);
void ss_timeout_apply(ui_state_t *ui);

/* Definidas en otras partes de Ajustes, pero usadas desde esta pagina. */
void create_sd_settings_page(ui_state_t *ui, lv_obj_t *page_sd);
void create_autostart_card(lv_obj_t *cont);
void populate_autocaravana(settings_page_ctx_t *ctx, lv_obj_t *page);

static void ss_mode_changed_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui) return;
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t mode = lv_dropdown_get_selected(dd);
    ui->screensaver.mode = mode;
    save_screensaver_mode(mode, ui->screensaver.rotate_period_min);
}

static void ss_period_dec_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui) return;
    if (ui->screensaver.rotate_period_min > 1) ui->screensaver.rotate_period_min--;
    save_screensaver_mode(ui->screensaver.mode, ui->screensaver.rotate_period_min);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (lbl) lv_label_set_text_fmt(lbl, "%d", ui->screensaver.rotate_period_min);
}

static void ss_period_inc_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui) return;
    if (ui->screensaver.rotate_period_min < 10) ui->screensaver.rotate_period_min++;
    save_screensaver_mode(ui->screensaver.mode, ui->screensaver.rotate_period_min);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (lbl) lv_label_set_text_fmt(lbl, "%d", ui->screensaver.rotate_period_min);
}

static void splash_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    save_splash_mode((uint8_t)idx);
    ESP_LOGI(TAG_SETTINGS, "Splash mode -> %u", (unsigned)idx);
}

void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display)
{
    style_settings_scrollbar(page_display);
    /* Root container */
    lv_obj_t *cont = lv_obj_create(page_display);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Brillo === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(100));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(0xBA68C8), 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_radius(card1, 12, 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_set_style_pad_gap(card1, 12, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Row: titulo a la izquierda, slider a la derecha */
    lv_obj_t *card1_row = lv_obj_create(card1);
    lv_obj_remove_style_all(card1_row);
    lv_obj_set_size(card1_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card1_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card1_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card1_title = lv_label_create(card1_row);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0xBA68C8), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_EYE_OPEN "  Brillo pantalla");

    /* Sub-row: valor + slider */
    lv_obj_t *card1_sub = lv_obj_create(card1_row);
    lv_obj_remove_style_all(card1_sub);
    lv_obj_set_size(card1_sub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(card1_sub, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1_sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card1_sub, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(card1_sub, 16, 0);
    lv_obj_set_style_pad_right(card1_sub, 16, 0);
    lv_obj_set_style_pad_gap(card1_sub, 10, 0);

    lv_obj_t *lbl_val_b = lv_label_create(card1_sub);
    lv_obj_set_style_text_font(lbl_val_b, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_val_b, lv_color_white(), 0);
    lv_obj_set_width(lbl_val_b, 70);
    lv_obj_set_style_text_align(lbl_val_b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(lbl_val_b, "%d%%", ui->brightness);

    lv_obj_t *slider_brightness = lv_slider_create(card1_sub);
    lv_obj_set_width(slider_brightness, 165);
    lv_obj_set_height(slider_brightness, 26);
    lv_obj_set_style_pad_right(card1_sub, 12, 0);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xBA68C8), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_brightness, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xBA68C8), LV_PART_KNOB);
    lv_slider_set_range(slider_brightness, 5, 100);
    /* Pasos de 5: snap del valor inicial al múltiplo más cercano (mínimo 5) */
    int b_init = ((ui->brightness + 2) / 5) * 5;
    if (b_init < 5) b_init = 5;
    if (b_init > 100) b_init = 100;
    if (b_init != ui->brightness) {
        ui->brightness = (uint8_t)b_init;
        lv_label_set_text_fmt(lbl_val_b, "%d%%", b_init);
    }
    lv_slider_set_value(slider_brightness, b_init, LV_ANIM_OFF);
    /* No tocar el brillo del sistema al construir el panel: produce un
     * parpadeo visible al cargar Settings (80% boot -> b_init -> 80% otra
     * vez). El brillo se aplica una sola vez al final del boot via
     * night_mode_timer_cb y luego cuando el usuario mueva el slider. */
    /* Helper: tag el label como user data secundaria via custom property */
    lv_obj_set_user_data(slider_brightness, lbl_val_b);
    /* VALUE_CHANGED: aplica brillo en vivo (sin tocar NVS). RELEASED: persiste
     * una sola vez al soltar (evita un nvs_commit por cada paso del arrastre). */
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_RELEASED, ui);

    /* === Card Modo nocturno (auto brillo por hora del RTC) === */
    lv_obj_t *card_nm = lv_obj_create(cont);
    lv_obj_set_width(card_nm, lv_pct(100));
    lv_obj_set_height(card_nm, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_nm, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_nm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_nm, lv_color_hex(0x9C27B0), 0);
    lv_obj_set_style_border_width(card_nm, 1, 0);
    lv_obj_set_style_radius(card_nm, 12, 0);
    lv_obj_set_style_pad_all(card_nm, 12, 0);
    lv_obj_set_style_pad_gap(card_nm, 14, 0);
    lv_obj_set_layout(card_nm, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_nm, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card_nm, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Todo en una linea con hijos DIRECTOS + spacer flexible: evita el bug de
     * LVGL de un contenedor SIZE_CONTENT anidado en un padre SPACE_BETWEEN
     * (colapsaba y recortaba el bloque -> se veia solo "Fin"). */
    lv_obj_t *nm_title = lv_label_create(card_nm);
    lv_obj_set_style_text_font(nm_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(nm_title, lv_color_hex(0x9C27B0), 0);
    lv_label_set_text(nm_title, LV_SYMBOL_EYE_CLOSE "  Modo nocturno");

    lv_obj_t *nm_sw = lv_switch_create(card_nm);
    lv_obj_set_style_bg_color(nm_sw, lv_color_hex(0x9C27B0),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ui->night_mode.enabled) lv_obj_add_state(nm_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(nm_sw, night_switch_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Spacer flexible: empuja los selectores Inicio/Fin al borde derecho */
    lv_obj_t *nm_spacer = lv_obj_create(card_nm);
    lv_obj_remove_style_all(nm_spacer);
    lv_obj_set_height(nm_spacer, 1);
    lv_obj_set_flex_grow(nm_spacer, 1);

    for (int slot = 0; slot < 2; slot++) {
        lv_obj_t *grp = lv_obj_create(card_nm);
        lv_obj_remove_style_all(grp);
        lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(grp, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(grp, 6, 0);

        lv_obj_t *cap = lv_label_create(grp);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_20_es, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xBBBBBB), 0);
        lv_label_set_text(cap, slot == 0 ? "Inicio" : "Fin");

        lv_obj_t *btn_dec = lv_btn_create(grp);
        lv_obj_set_size(btn_dec, 34, 34);
        lv_obj_set_style_bg_color(btn_dec, lv_color_hex(0x9C27B0), 0);
        lv_obj_set_style_radius(btn_dec, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(btn_dec, 0, 0);
        lv_obj_t *bd = lv_label_create(btn_dec);
        lv_label_set_text(bd, "-");
        lv_obj_set_style_text_font(bd, &lv_font_montserrat_24_es, 0);
        lv_obj_center(bd);

        lv_obj_t *val = lv_label_create(grp);
        lv_obj_set_style_text_font(val, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(val, lv_color_white(), 0);
        uint8_t h = slot == 0 ? ui->night_mode.start_h : ui->night_mode.end_h;
        lv_label_set_text_fmt(val, "%02u:00", h);
        lv_obj_set_width(val, 70);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *btn_inc = lv_btn_create(grp);
        lv_obj_set_size(btn_inc, 34, 34);
        lv_obj_set_style_bg_color(btn_inc, lv_color_hex(0x9C27B0), 0);
        lv_obj_set_style_radius(btn_inc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(btn_inc, 0, 0);
        lv_obj_t *bi = lv_label_create(btn_inc);
        lv_label_set_text(bi, "+");
        lv_obj_set_style_text_font(bi, &lv_font_montserrat_24_es, 0);
        lv_obj_center(bi);

        lv_obj_set_user_data(btn_dec, val);
        lv_obj_set_user_data(btn_inc, val);
        if (slot == 0) {
            lv_obj_add_event_cb(btn_dec, night_start_dec_cb, LV_EVENT_CLICKED, ui);
            lv_obj_add_event_cb(btn_inc, night_start_inc_cb, LV_EVENT_CLICKED, ui);
        } else {
            lv_obj_add_event_cb(btn_dec, night_end_dec_cb, LV_EVENT_CLICKED, ui);
            lv_obj_add_event_cb(btn_inc, night_end_inc_cb, LV_EVENT_CLICKED, ui);
        }
    }

    /* === Fila con dos cards al 49%: Vista por defecto + Pantalla de bienvenida ===
     * No caben en una sola linea fisica (los dos titulos + desplegables se pasan
     * de ancho), asi que van como dos cards lado a lado, cada una con su titulo
     * arriba y el desplegable debajo. */
    lv_obj_t *row_views = lv_obj_create(cont);
    lv_obj_remove_style_all(row_views);
    lv_obj_set_width(row_views, lv_pct(100));
    lv_obj_set_height(row_views, LV_SIZE_CONTENT);
    lv_obj_set_layout(row_views, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_views, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_views, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* === Card Vista por defecto (49%, izda) === */
    lv_obj_t *card3 = lv_obj_create(row_views);
    lv_obj_set_width(card3, lv_pct(49));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 12, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card3_title = lv_label_create(card3);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_LIST "  Vista por defecto");

    ui->view_selection.dropdown = lv_dropdown_create(card3);
    lv_obj_set_width(ui->view_selection.dropdown, lv_pct(100));
    lv_dropdown_set_options(ui->view_selection.dropdown,
        "Auto Detection\n"
        "Default Battery View\n"
        "Solar Charger View\n"
        "Battery Monitor View\n"
        "Inverter View\n"
        "DC/DC Converter View\n"
        "Overview"
    );
    uint8_t saved_mode = (uint8_t)UI_VIEW_MODE_OVERVIEW;
    if (load_ui_view_mode(&saved_mode) == ESP_OK) {
        ui->view_selection.mode = (ui_view_mode_t)saved_mode;
    } else {
        ui->view_selection.mode = UI_VIEW_MODE_OVERVIEW;
    }
    lv_dropdown_set_selected(ui->view_selection.dropdown, (uint16_t)ui->view_selection.mode);
    lv_obj_add_event_cb(ui->view_selection.dropdown, view_selection_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* === Card Splash / Pantalla de bienvenida (49%, dcha) === */
    lv_obj_t *card_sp = lv_obj_create(row_views);
    lv_obj_set_width(card_sp, lv_pct(49));
    lv_obj_set_height(card_sp, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_sp, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_sp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_sp, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card_sp, 1, 0);
    lv_obj_set_style_radius(card_sp, 12, 0);
    lv_obj_set_style_pad_all(card_sp, 16, 0);
    lv_obj_set_style_pad_gap(card_sp, 12, 0);
    lv_obj_set_layout(card_sp, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_sp, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *sp_title = lv_label_create(card_sp);
    lv_obj_set_style_text_font(sp_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(sp_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(sp_title, LV_SYMBOL_IMAGE "  Pantalla de bienvenida");

    lv_obj_t *sp_dd = lv_dropdown_create(card_sp);
    lv_obj_set_width(sp_dd, lv_pct(100));
    lv_dropdown_set_options(sp_dd, "Sin splash\nLogo furgo");
    {
        uint8_t m = 1;
        load_splash_mode(&m);
        lv_dropdown_set_selected(sp_dd, m > 1 ? 1 : m);
    }
    lv_obj_add_event_cb(sp_dd, splash_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* === Sub-bloque Salvapantallas dentro del mismo card === */
    /* Separador fino sutil entre Brillo y Salvapantallas */
    lv_obj_t *card1_sep = lv_obj_create(card1);
    lv_obj_remove_style_all(card1_sep);
    lv_obj_set_size(card1_sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(card1_sep, lv_color_hex(0x2D3340), 0);
    lv_obj_set_style_bg_opa(card1_sep, LV_OPA_COVER, 0);

    /* Contenedor del sub-bloque Screensaver (sin estilo propio: hereda del card1) */
    lv_obj_t *card2 = lv_obj_create(card1);
    lv_obj_remove_style_all(card2);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(card2, 12, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);

    /* Fila titulo: [EYE_CLOSE Salvapantallas (flex_grow)] [Activar] [SW] [Tiempo - 1 +]
     * Estructura plana: title con flex_grow=1 empuja el resto a la dcha. */
    lv_obj_t *title_row = lv_obj_create(card2);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(title_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Separacion entre 'Salvapantallas' y el switch/Tiempo */
    lv_obj_set_style_pad_column(title_row, 28, 0);

    lv_obj_t *card2_title = lv_label_create(title_row);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_EYE_CLOSE "  Salvapantallas");

    /* Switch JUNTO al titulo (sin label, mismo estilo que Modo nocturno). */
    ui->screensaver.checkbox = lv_switch_create(title_row);
    lv_obj_set_size(ui->screensaver.checkbox, 50, 28);
    lv_obj_set_style_bg_color(ui->screensaver.checkbox, lv_color_hex(0xFF9800),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ui->screensaver.enabled) lv_obj_add_state(ui->screensaver.checkbox, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ui->screensaver.checkbox, cb_screensaver_event_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Spacer invisible flex_grow=1: empuja el cont_to hacia la derecha
     * dejando el switch pegado al titulo. */
    lv_obj_t *spacer = lv_obj_create(title_row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    /* Tiempo (min): label + [-][spin][+] (los botones se anaden mas abajo
     * a cont_to, no aqui). */
    lv_obj_t *cont_to = lv_obj_create(title_row);
    lv_obj_remove_style_all(cont_to);
    lv_obj_set_size(cont_to, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(cont_to, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont_to, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(cont_to, 8, 0);
    lv_obj_set_flex_align(cont_to, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_to_inline = lv_label_create(cont_to);
    lv_obj_set_style_text_font(lbl_to_inline, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_to_inline, "Tiempo (min):");

    /* Brillo SS - en row */
    lv_obj_t *row_ss_b = lv_obj_create(card2);
    lv_obj_remove_style_all(row_ss_b);
    lv_obj_set_size(row_ss_b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_ss_b, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_ss_b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_ss_b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_ss_b, 16, 0);

    lv_obj_t *lbl_ss = lv_label_create(row_ss_b);
    lv_obj_set_style_text_font(lbl_ss, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_ss, "Brillo en reposo:");

    lv_obj_t *lbl_val_ss = lv_label_create(row_ss_b);
    lv_obj_set_style_text_font(lbl_val_ss, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_val_ss, lv_color_white(), 0);
    lv_obj_set_width(lbl_val_ss, 70);
    lv_obj_set_style_text_align(lbl_val_ss, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(lbl_val_ss, "%d%%", ui->screensaver.brightness);

    ui->screensaver.slider_brightness = lv_slider_create(row_ss_b);

    lv_obj_set_height(ui->screensaver.slider_brightness, 26);
    lv_obj_set_style_bg_color(ui->screensaver.slider_brightness, lv_color_hex(0xFF9800), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui->screensaver.slider_brightness, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui->screensaver.slider_brightness, lv_color_hex(0xFF9800), LV_PART_KNOB);
    lv_slider_set_range(ui->screensaver.slider_brightness, 0, ui->brightness);
    if (ui->screensaver.brightness > ui->brightness) ui->screensaver.brightness = ui->brightness;
    /* Pasos de 5: snap del valor inicial al múltiplo más cercano */
    int ss_init = ((ui->screensaver.brightness + 2) / 5) * 5;
    if (ss_init > ui->brightness) ss_init = ui->brightness;
    if (ss_init < 0) ss_init = 0;
    if (ss_init != ui->screensaver.brightness) {
        ui->screensaver.brightness = (uint8_t)ss_init;
        lv_label_set_text_fmt(lbl_val_ss, "%d%%", ss_init);
    }
    lv_slider_set_value(ui->screensaver.slider_brightness, ss_init, LV_ANIM_OFF);
    lv_obj_set_user_data(ui->screensaver.slider_brightness, lbl_val_ss);
    lv_obj_add_event_cb(ui->screensaver.slider_brightness, slider_ss_brightness_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->screensaver.slider_brightness, slider_ss_brightness_event_cb, LV_EVENT_RELEASED, ui);

    /* +/- y label dentro de cont_to (mismo estilo que el selector de tiempo del modo) */
    lv_obj_t *btn_dec = lv_btn_create(cont_to);
    lv_obj_set_size(btn_dec, 40, 40);
    lv_obj_set_style_bg_color(btn_dec, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_dec, 8, 0);
    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_dec);
    lv_obj_add_event_cb(btn_dec, spinbox_ss_time_decrement_event_cb, LV_EVENT_CLICKED, ui);

    /* Label central (reutilizamos el campo spinbox_timeout como lv_obj_t* genérico) */
    ui->screensaver.spinbox_timeout = lv_label_create(cont_to);
    lv_obj_set_style_text_font(ui->screensaver.spinbox_timeout, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ui->screensaver.spinbox_timeout, lv_color_white(), 0);
    lv_obj_set_width(ui->screensaver.spinbox_timeout, 60);
    lv_obj_set_style_text_align(ui->screensaver.spinbox_timeout, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(ui->screensaver.spinbox_timeout, "%d", ui->screensaver.timeout / 60);

    lv_obj_t *btn_inc = lv_btn_create(cont_to);
    lv_obj_set_size(btn_inc, 40, 40);
    lv_obj_set_style_bg_color(btn_inc, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(btn_inc, 8, 0);
    lv_obj_t *lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_inc);
    lv_obj_add_event_cb(btn_inc, spinbox_ss_time_increment_event_cb, LV_EVENT_CLICKED, ui);

    /* Row UNICA: Modo + Tiempo por vista */
    lv_obj_t *row_mode = lv_obj_create(card2);
    lv_obj_remove_style_all(row_mode);
    lv_obj_set_size(row_mode, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_mode, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_mode, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mode, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_mode, 12, 0);

    lv_obj_t *lbl_mode = lv_label_create(row_mode);
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_mode, "Modo:");

    lv_obj_t *dd_mode = lv_dropdown_create(row_mode);
    lv_dropdown_set_options(dd_mode, "Atenuar\nRotar vistas");
    lv_obj_set_width(dd_mode, 200);
    lv_dropdown_set_selected(dd_mode, ui->screensaver.mode);
    lv_obj_add_event_cb(dd_mode, ss_mode_changed_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Sub-grupo: label + selector juntos */
    lv_obj_t *grp_period = lv_obj_create(row_mode);
    lv_obj_remove_style_all(grp_period);
    lv_obj_set_size(grp_period, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(grp_period, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grp_period, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(grp_period, 10, 0);
    lv_obj_set_flex_align(grp_period, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_period = lv_label_create(grp_period);
    lv_obj_set_style_text_font(lbl_period, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_period, "Tiempo (min):");

    lv_obj_t *cont_period = lv_obj_create(grp_period);
    lv_obj_remove_style_all(cont_period);
    lv_obj_set_size(cont_period, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(cont_period, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont_period, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(cont_period, 8, 0);
    lv_obj_set_flex_align(cont_period, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_period_dec = lv_btn_create(cont_period);
    lv_obj_set_size(btn_period_dec, 40, 40);
    lv_obj_set_style_bg_color(btn_period_dec, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_period_dec, 8, 0);
    lv_obj_t *lbl_pdec = lv_label_create(btn_period_dec);
    lv_label_set_text(lbl_pdec, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_pdec);
    lv_obj_add_event_cb(btn_period_dec, ss_period_dec_cb, LV_EVENT_CLICKED, ui);

    lv_obj_t *lbl_period_val = lv_label_create(cont_period);
    lv_obj_set_style_text_font(lbl_period_val, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_period_val, lv_color_white(), 0);
    lv_obj_set_width(lbl_period_val, 60);
    lv_obj_set_style_text_align(lbl_period_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(lbl_period_val, "%d", ui->screensaver.rotate_period_min);
    lv_obj_set_user_data(btn_period_dec, lbl_period_val);

    lv_obj_t *btn_period_inc = lv_btn_create(cont_period);
    lv_obj_set_size(btn_period_inc, 40, 40);
    lv_obj_set_style_bg_color(btn_period_inc, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(btn_period_inc, 8, 0);
    lv_obj_t *lbl_pinc = lv_label_create(btn_period_inc);
    lv_label_set_text(lbl_pinc, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_pinc);
    lv_obj_add_event_cb(btn_period_inc, ss_period_inc_cb, LV_EVENT_CLICKED, ui);
    lv_obj_set_user_data(btn_period_inc, lbl_period_val);

    /* (El card "Carrusel captura pantalla" + visor se movio a su propia pagina
     *  de Settings "Tarjeta SD": create_sd_settings_page.) */

    /* (La card "Auto-encendido (luz + bomba)" se movio al submenu Autocaravana:
     *  ver create_autostart_card / populate_autocaravana.) */
}

static void brightness_slider_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    /* Snap a múltiplos de 5 (rango 5..100) */
    int snapped = ((val + 2) / 5) * 5;
    if (snapped < 5) snapped = 5;
    if (snapped > 100) snapped = 100;
    if (snapped != val) {
        lv_slider_set_value(slider, snapped, LV_ANIM_OFF);
        val = snapped;
    }
    /* Persistir en NVS solo al soltar (RELEASED); en VALUE_CHANGED solo brillo vivo. */
    bool persist = (lv_event_get_code(e) == LV_EVENT_RELEASED);
    ui->brightness = (uint8_t)val;
    if (persist) {
        save_brightness(ui->brightness);
        ESP_LOGI(TAG_SETTINGS, "Brightness set to %d", val);
    }
    apply_brightness_for_now(ui);   /* respeta noche si estamos en franja */
    /* Update label */
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", val);
    /* Ajustar máximo del slider screensaver al nuevo brightness */
    if (ui->screensaver.slider_brightness != NULL) {
        int ss_val = lv_slider_get_value(ui->screensaver.slider_brightness);
        lv_slider_set_range(ui->screensaver.slider_brightness, 0, val);
        /* Si el valor actual supera el nuevo máximo, lo recortamos */
        if (ss_val > val) {
            lv_slider_set_value(ui->screensaver.slider_brightness, val, LV_ANIM_OFF);
            ui->screensaver.brightness = (uint8_t)val;
            if (persist) save_screensaver_settings(ui->screensaver.enabled, ui->screensaver.brightness, ui->screensaver.timeout);
        }
    }
}

static void night_switch_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    lv_obj_t *sw = lv_event_get_target(e);
    ui->night_mode.enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    night_save_and_apply(ui);
}

static void night_start_dec_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    ui->night_mode.start_h = (ui->night_mode.start_h + 23) % 24;
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lbl) lv_label_set_text_fmt(lbl, "%02u:00", ui->night_mode.start_h);
    night_save_and_apply(ui);
}

static void night_start_inc_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    ui->night_mode.start_h = (ui->night_mode.start_h + 1) % 24;
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lbl) lv_label_set_text_fmt(lbl, "%02u:00", ui->night_mode.start_h);
    night_save_and_apply(ui);
}

static void night_end_dec_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    ui->night_mode.end_h = (ui->night_mode.end_h + 23) % 24;
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lbl) lv_label_set_text_fmt(lbl, "%02u:00", ui->night_mode.end_h);
    night_save_and_apply(ui);
}

static void night_end_inc_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    ui->night_mode.end_h = (ui->night_mode.end_h + 1) % 24;
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lbl) lv_label_set_text_fmt(lbl, "%02u:00", ui->night_mode.end_h);
    night_save_and_apply(ui);
}

static void cb_screensaver_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.checkbox == NULL) {
        return;
    }
    ui->screensaver.enabled = lv_obj_has_state(ui->screensaver.checkbox, LV_STATE_CHECKED);
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    screensaver_enable(ui, ui->screensaver.enabled);
}

static void slider_ss_brightness_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.slider_brightness == NULL) {
        return;
    }
    int v = lv_slider_get_value(ui->screensaver.slider_brightness);
    /* Snap a múltiplos de 5 (rango 0..ui->brightness) */
    int snapped = ((v + 2) / 5) * 5;
    if (snapped < 0) snapped = 0;
    if (snapped > ui->brightness) snapped = ui->brightness;
    if (snapped != v) {
        lv_slider_set_value(ui->screensaver.slider_brightness, snapped, LV_ANIM_OFF);
        v = snapped;
    }
    ui->screensaver.brightness = v;
    /* Persistir solo al soltar (RELEASED); en VALUE_CHANGED solo vista previa. */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        save_screensaver_settings(ui->screensaver.enabled,
                                  ui->screensaver.brightness,
                                  ui->screensaver.timeout);
    }
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(ui->screensaver.slider_brightness);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", v);
    if (ui->screensaver.active) {
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
    }
}

static void spinbox_ss_time_increment_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) return;
    int min = ui->screensaver.timeout / 60;
    if (min < 30) min++;
    ui->screensaver.timeout = min * 60;
    ss_timeout_apply(ui);
}

static void spinbox_ss_time_decrement_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) return;
    int min = ui->screensaver.timeout / 60;
    if (min > 1) min--;   /* minimo 1 min; para desactivar, el interruptor ON/OFF */
    ui->screensaver.timeout = min * 60;
    ss_timeout_apply(ui);
}

static void view_selection_dropdown_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    if (selected < UI_VIEW_MODE_COUNT) {
        ui->view_selection.mode = (ui_view_mode_t)selected;
        
        /* Save the selection */
        esp_err_t err = save_ui_view_mode((uint8_t)ui->view_selection.mode);
        if (err == ESP_OK) {
            ESP_LOGI(TAG_SETTINGS, "UI view mode set to %d", (int)ui->view_selection.mode);
        } else {
            ESP_LOGW(TAG_SETTINGS, "Failed to save UI view mode: %s", esp_err_to_name(err));
        }
        
        /* Force a view update to apply the new selection */
        ui_force_view_update();
    }
}
