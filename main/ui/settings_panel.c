#include "settings_panel.h"
#include "ui.h"
#include "ui_card.h"
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
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "watchdog.h"
#include "config_backup.h"
#include "trip_computer.h"
#include <time.h>

// Forward declaration for view update function
extern void ui_force_view_update(void);

#define WIFI_NAMESPACE "wifi"

static const char *TAG_SETTINGS = "UI_SETTINGS";
static const char *APP_VERSION = "1.3.0";

static void ta_event_cb(lv_event_t *e);
static void wifi_event_cb(lv_event_t *e);
static void password_toggle_btn_event_cb(lv_event_t *e);
static void about_refresh_dynamic(ui_state_t *ui);
static void about_timer_cb(lv_timer_t *t);
static void reboot_msgbox_cb(lv_event_t *e);
static void reboot_btn_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);
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

static void cb_screensaver_event_cb(lv_event_t *e);
static void slider_ss_brightness_event_cb(lv_event_t *e);
static void spinbox_ss_time_increment_event_cb(lv_event_t *e);
static void spinbox_ss_time_decrement_event_cb(lv_event_t *e);
static void screensaver_timer_cb(lv_timer_t *timer);
static void view_selection_dropdown_event_cb(lv_event_t *e);

static void screensaver_enable(ui_state_t *ui, bool enable);
static void screensaver_wake(ui_state_t *ui);

/* ── Modo nocturno (auto brillo por hora del RTC) ─────────────── */
static void night_switch_cb(lv_event_t *e);
static void night_start_dec_cb(lv_event_t *e);
static void night_start_inc_cb(lv_event_t *e);
static void night_end_dec_cb(lv_event_t *e);
static void night_end_inc_cb(lv_event_t *e);
static void night_brightness_slider_cb(lv_event_t *e);

/* Aplica inmediatamente el brillo correcto según hora actual + config. */
static bool night_in_window(int h, uint8_t s, uint8_t e)
{
    if (s == e) return false;
    if (s < e)  return h >= s && h < e;
    return h >= s || h < e;     /* cruza medianoche */
}

static void apply_brightness_for_now(ui_state_t *ui)
{
    if (!ui) return;
    if (ui->screensaver.active) return;       /* el SS gestiona su brillo */
    int target = ui->brightness;
    if (ui->night_mode.enabled) {
        time_t now = time(NULL);
        if (now >= 1000000000L) {
            struct tm tm_local;
            localtime_r(&now, &tm_local);
            if (night_in_window(tm_local.tm_hour,
                                ui->night_mode.start_h,
                                ui->night_mode.end_h)) {
                target = ui->night_mode.brightness;
            }
        }
    }
    bsp_display_brightness_set(target);
}


// Victron devices configuration functions
static void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron);
static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page_about);
static void create_logs_settings_page(ui_state_t *ui, lv_obj_t *page);

/* ── Scrollbar visible en cualquier pagina de Settings ────────── */
/* Aplica scrollbar AUTO (visible cuando hay overflow) con estilo claro
 * para indicar que se puede deslizar. Llamar tras crear la page. */
static void style_settings_scrollbar(lv_obj_t *page)
{
    if (!page) return;
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xFF9800), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(page, 6, LV_PART_SCROLLBAR);
}

/* === SoC umbrales (dropdowns) === */
static const int s_soc_crit_options[] = { 10, 20, 30, 40 };
static const int s_soc_warn_options[] = { 40, 50, 60, 70 };

static void soc_crit_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0])) {
        alerts_set_soc_critical(s_soc_crit_options[sel]);
    }
}

static void soc_warn_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0])) {
        alerts_set_soc_warning(s_soc_warn_options[sel]);
    }
}

static void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page);
static void alarm_min_dd_cb_sound(lv_event_t *e);
static void alarm_temp_dd_cb_sound(lv_event_t *e);



static lv_obj_t *s_ap_msgbox = NULL;
static ui_state_t *s_ap_dialog_ui = NULL;

static void ap_msgbox_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    if (txt && strstr(txt, "Reiniciar")) {
        /* Cerrar modal antes del restart para evitar glitch visual */
        if (s_ap_msgbox) { lv_obj_del(s_ap_msgbox); s_ap_msgbox = NULL; }
        /* Splash "Reiniciando..." pantalla completa en negro */
        lv_obj_t *splash = lv_obj_create(lv_layer_top());
        lv_obj_set_size(splash, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(splash, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(splash, 0, 0);
        lv_obj_set_style_pad_all(splash, 0, 0);
        lv_obj_t *spl_lbl = lv_label_create(splash);
        lv_obj_set_style_text_font(spl_lbl, &lv_font_montserrat_28_es, 0);
        lv_obj_set_style_text_color(spl_lbl, lv_color_white(), 0);
        lv_label_set_text(spl_lbl, LV_SYMBOL_POWER "  Reiniciando...");
        lv_obj_center(spl_lbl);
        lv_refr_now(NULL);

        ESP_LOGI("UI", "Flushing data before restart...");
        datalogger_flush();
        battery_history_flush();
        /* Apagar backlight para que la pantalla quede negra entre el reset
         * y la reinicialización del panel DSI (evita el flash azul del
         * buffer DSI sin inicializar). */
        bsp_display_brightness_set(0);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else {
        /* Cancelar: revertir NVS al estado opuesto y sincronizar el switch */
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
            uint8_t cur = 1;
            nvs_get_u8(h, "enabled", &cur);
            uint8_t reverted = cur ? 0 : 1;
            nvs_set_u8(h, "enabled", reverted);
            nvs_commit(h);
            nvs_close(h);
            if (s_ap_dialog_ui && s_ap_dialog_ui->wifi.ap_enable) {
                if (reverted) lv_obj_add_state(s_ap_dialog_ui->wifi.ap_enable, LV_STATE_CHECKED);
                else          lv_obj_clear_state(s_ap_dialog_ui->wifi.ap_enable, LV_STATE_CHECKED);
            }
        }
    }
    if (s_ap_msgbox) {
        lv_obj_del(s_ap_msgbox);
        s_ap_msgbox = NULL;
    }
    s_ap_dialog_ui = NULL;
}

void ui_show_wifi_restart_dialog(ui_state_t *ui)
{
    s_ap_dialog_ui = ui;

    /* Fondo modal semi-transparente */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_ap_msgbox = modal;

    /* Card de dialogo */
    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 520, 240);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Cambio en Wi-Fi");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Aplicar el cambio requiere reiniciar el dispositivo.");

    /* Row de botones */
    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 200, 60);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, ap_msgbox_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_restart = lv_btn_create(row_btns);
    lv_obj_set_size(btn_restart, 200, 60);
    lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_restart, 12, 0);
    lv_obj_t *lr = lv_label_create(btn_restart);
    lv_label_set_text(lr, LV_SYMBOL_POWER "  Reiniciar");
    lv_obj_set_style_text_font(lr, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lr);
    lv_obj_add_event_cb(btn_restart, ap_msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void ap_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = lv_event_get_target(e);
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    /* Guardar el nuevo estado en NVS; si cancela, el modal lo revertirá. */
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", checked ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    ui_show_wifi_restart_dialog(ui);
}

static void portal_page_cb(lv_event_t *e);
static void victron_config_add_btn_event_cb(lv_event_t *e);
static void victron_config_remove_btn_event_cb(lv_event_t *e);
static void victron_config_create_row(ui_state_t *ui, size_t index);
static void victron_config_update_controls(ui_state_t *ui);
static void victron_config_persist(ui_state_t *ui);
static void victron_config_load(ui_state_t *ui);
static void victron_config_refresh(ui_state_t *ui);
static void victron_enabled_checkbox_event_cb(lv_event_t *e);
static void victron_field_ta_event_cb(lv_event_t *e);
static void victron_config_update_device_status(ui_state_t *ui, const char *mac_address, 
                                                const char *device_type, const char *product_name, 
                                                const char *error_info);
static int victron_config_find_device_by_mac(ui_state_t *ui, const char *mac_address);


/* --- Estilo botones del menu Settings --- */
static lv_obj_t *s_settings_menu = NULL;
static lv_obj_t *s_settings_main_page = NULL;
static lv_style_t s_settings_btn_style;
static lv_style_t s_settings_btn_pressed_style;
static bool s_settings_styles_inited = false;

void ui_settings_panel_go_to_main(void)
{
    if (s_settings_menu && s_settings_main_page) {
        lv_menu_set_page(s_settings_menu, s_settings_main_page);
    }
}

static void settings_btn_styles_init(void);
static void settings_menu_add_entry(ui_state_t *ui, lv_obj_t *main_page,
                                    lv_obj_t *menu, lv_obj_t *target_page,
                                    const char *text);

static void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                                      const char *default_ssid,
                                      const char *default_pass,
                                      uint8_t ap_enabled)
{
    (void)ap_enabled;
    style_settings_scrollbar(page_wifi);
    /* Root container */
    lv_obj_t *cont = lv_obj_create(page_wifi);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Punto de acceso === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(100));
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
    lv_obj_add_event_cb(ui->wifi.ssid, wifi_event_cb, LV_EVENT_VALUE_CHANGED, ui);

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

    const char *ap_password = (default_pass && default_pass[0] != '\0') ? default_pass : DEFAULT_AP_PASSWORD;

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
    lv_obj_add_event_cb(ui->wifi.password, wifi_event_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* === Card 2: Portal web === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 12, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_LIST "  Pagina inicial portal");

    /* Dropdown: 0=Keys, 1=Logs, 2=Dashboard */
    lv_obj_t *dd_portal = lv_dropdown_create(card2);
    lv_obj_set_width(dd_portal, 200);
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
}

/* ── Zona horaria: presets + callback dropdown ────────────────────────── */
static const char *TZ_LABELS =
    "Madrid (CET/CEST)\n"
    "Lisboa / Canarias (WET/WEST)\n"
    "Londres (GMT/BST)\n"
    "Berlin (CET/CEST)\n"
    "Atenas (EET/EEST)\n"
    "UTC";

static const char *TZ_POSIX[] = {
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "WET0WEST,M3.5.0/1,M10.5.0",
    "GMT0BST,M3.5.0/1,M10.5.0",
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "UTC0",
};
#define TZ_COUNT (sizeof(TZ_POSIX)/sizeof(TZ_POSIX[0]))

static int tz_index_from_posix(const char *cur)
{
    for (int i = 0; i < (int)TZ_COUNT; i++) {
        if (strcmp(cur, TZ_POSIX[i]) == 0) return i;
    }
    return 0; /* default Madrid */
}

static void tz_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    if (idx >= TZ_COUNT) return;
    const char *posix = TZ_POSIX[idx];
    save_timezone(posix);
    setenv("TZ", posix, 1);
    tzset();
    ESP_LOGI(TAG_SETTINGS, "TZ -> %s", posix);
}

/* ── Splash dropdown ──────────────────────────────────────────── */
static void splash_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    save_splash_mode((uint8_t)idx);
    ESP_LOGI(TAG_SETTINGS, "Splash mode -> %u", (unsigned)idx);
}

static void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display)
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
    lv_obj_set_style_border_color(card1, lv_color_hex(0x4FC3F7), 0);
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
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0x4FC3F7), 0);
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
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_brightness, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0x4FC3F7), LV_PART_KNOB);
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
    bsp_display_brightness_set(b_init);
    /* Helper: tag el label como user data secundaria via custom property */
    lv_obj_set_user_data(slider_brightness, lbl_val_b);
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, ui);

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
    lv_obj_set_style_pad_gap(card_nm, 8, 0);
    lv_obj_set_layout(card_nm, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_nm, LV_FLEX_FLOW_COLUMN);

    /* Row 1: titulo + switch + "Brillo nocturno" + valor + slider */
    lv_obj_t *nm_top = lv_obj_create(card_nm);
    lv_obj_remove_style_all(nm_top);
    lv_obj_set_size(nm_top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(nm_top, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nm_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nm_top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nm_top, 10, 0);

    /* Bloque izquierdo: titulo + switch */
    lv_obj_t *nm_lhs = lv_obj_create(nm_top);
    lv_obj_remove_style_all(nm_lhs);
    lv_obj_set_size(nm_lhs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(nm_lhs, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nm_lhs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nm_lhs, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nm_lhs, 10, 0);

    lv_obj_t *nm_title = lv_label_create(nm_lhs);
    lv_obj_set_style_text_font(nm_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(nm_title, lv_color_hex(0x9C27B0), 0);
    lv_label_set_text(nm_title, LV_SYMBOL_EYE_CLOSE "  Modo nocturno");

    lv_obj_t *nm_sw = lv_switch_create(nm_lhs);
    lv_obj_set_style_bg_color(nm_sw, lv_color_hex(0x9C27B0),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ui->night_mode.enabled) lv_obj_add_state(nm_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(nm_sw, night_switch_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Bloque derecho: Brillo nocturno (label + valor + slider) en la MISMA fila */
    lv_obj_t *nm_bri_grp = lv_obj_create(nm_top);
    lv_obj_remove_style_all(nm_bri_grp);
    lv_obj_set_size(nm_bri_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(nm_bri_grp, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nm_bri_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nm_bri_grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nm_bri_grp, 10, 0);

    lv_obj_t *nm_bri_lbl = lv_label_create(nm_bri_grp);
    lv_obj_set_style_text_font(nm_bri_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(nm_bri_lbl, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(nm_bri_lbl, "Brillo nocturno");

    lv_obj_t *nm_bri_val = lv_label_create(nm_bri_grp);
    lv_obj_set_style_text_font(nm_bri_val, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(nm_bri_val, lv_color_white(), 0);
    lv_obj_set_width(nm_bri_val, 60);
    lv_obj_set_style_text_align(nm_bri_val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(nm_bri_val, "%d%%", ui->night_mode.brightness);

    lv_obj_t *nm_bri_slider = lv_slider_create(nm_bri_grp);
    lv_obj_set_width(nm_bri_slider, 200);
    lv_obj_set_height(nm_bri_slider, 22);
    lv_slider_set_range(nm_bri_slider, 5, 100);
    lv_slider_set_value(nm_bri_slider, ui->night_mode.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(nm_bri_slider, lv_color_hex(0x9C27B0),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(nm_bri_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(nm_bri_slider, lv_color_hex(0x9C27B0), LV_PART_KNOB);
    lv_obj_set_user_data(nm_bri_slider, nm_bri_val);
    lv_obj_add_event_cb(nm_bri_slider, night_brightness_slider_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    /* Row 2: selectores Inicio + Fin (inline) */
    lv_obj_t *nm_hours = lv_obj_create(card_nm);
    lv_obj_remove_style_all(nm_hours);
    lv_obj_set_size(nm_hours, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(nm_hours, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nm_hours, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nm_hours, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nm_hours, 12, 0);

    for (int slot = 0; slot < 2; slot++) {
        lv_obj_t *grp = lv_obj_create(nm_hours);
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

    /* === Card Zona horaria === */
    lv_obj_t *card_tz = lv_obj_create(cont);
    lv_obj_set_width(card_tz, lv_pct(100));
    lv_obj_set_height(card_tz, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_tz, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_tz, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_tz, lv_color_hex(0x00C851), 0);  /* verde */
    lv_obj_set_style_border_width(card_tz, 1, 0);
    lv_obj_set_style_radius(card_tz, 12, 0);
    lv_obj_set_style_pad_all(card_tz, 16, 0);
    lv_obj_set_style_pad_gap(card_tz, 10, 0);
    lv_obj_set_layout(card_tz, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_tz, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card_tz, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tz_title = lv_label_create(card_tz);
    lv_obj_set_style_text_font(tz_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(tz_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(tz_title, LV_SYMBOL_GPS "  Zona horaria");

    lv_obj_t *tz_dd = lv_dropdown_create(card_tz);
    lv_obj_set_width(tz_dd, 320);
    lv_dropdown_set_options(tz_dd, TZ_LABELS);
    {
        char tz_now[48];
        load_timezone(tz_now, sizeof(tz_now));
        lv_dropdown_set_selected(tz_dd, tz_index_from_posix(tz_now));
    }
    lv_obj_add_event_cb(tz_dd, tz_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* === Card Splash (logo de bienvenida al boot) === */
    lv_obj_t *card_sp = lv_obj_create(cont);
    lv_obj_set_width(card_sp, lv_pct(100));
    lv_obj_set_height(card_sp, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_sp, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_sp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_sp, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card_sp, 1, 0);
    lv_obj_set_style_radius(card_sp, 12, 0);
    lv_obj_set_style_pad_all(card_sp, 16, 0);
    lv_obj_set_style_pad_gap(card_sp, 10, 0);
    lv_obj_set_layout(card_sp, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_sp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card_sp, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sp_title = lv_label_create(card_sp);
    lv_obj_set_style_text_font(sp_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(sp_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(sp_title, LV_SYMBOL_IMAGE "  Pantalla de bienvenida");

    lv_obj_t *sp_dd = lv_dropdown_create(card_sp);
    lv_obj_set_width(sp_dd, 220);
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

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_EYE_CLOSE "  Salvapantallas");

    /* Row: Activar checkbox + Tiempo spinbox en la misma linea */
    lv_obj_t *row_enable_to = lv_obj_create(card2);
    lv_obj_remove_style_all(row_enable_to);
    lv_obj_set_size(row_enable_to, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_enable_to, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_enable_to, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_enable_to, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Enable */
    ui->screensaver.checkbox = lv_checkbox_create(row_enable_to);
    lv_checkbox_set_text(ui->screensaver.checkbox, "Activar");
    lv_obj_set_style_text_font(ui->screensaver.checkbox, &lv_font_montserrat_20_es, 0);
    if (ui->screensaver.enabled) lv_obj_add_state(ui->screensaver.checkbox, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ui->screensaver.checkbox, cb_screensaver_event_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Tiempo (min): label + [-][spin][+] */
    lv_obj_t *cont_to = lv_obj_create(row_enable_to);
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

    /* === Card 3: Modo de vista === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
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

    lv_obj_t *card3_row = lv_obj_create(card3);
    lv_obj_remove_style_all(card3_row);
    lv_obj_set_size(card3_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card3_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3_row);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_LIST "  Vista por defecto");

    ui->view_selection.dropdown = lv_dropdown_create(card3_row);
    lv_obj_set_width(ui->view_selection.dropdown, 280);
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
}


static void victron_keys_show_warning(ui_state_t *ui);
static void victron_warning_btn_cb(lv_event_t *e);
static void victron_keys_clicked_cb(lv_event_t *e);
static void victron_keys_clicked_cb(lv_event_t *e)
{
    ui_state_t *u = (ui_state_t *)lv_event_get_user_data(e);
    victron_keys_show_warning(u);
}


static lv_obj_t *s_victron_warning = NULL;

static void victron_keys_show_warning(ui_state_t *ui)
{
    (void)ui;
    if (s_victron_warning) return;
    /* Modal background */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_victron_warning = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  Atencion");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Los cambios en esta seccion pueden afectar al funcionamiento del sistema. Procede con cuidado.");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 220, 60);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, victron_warning_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 220, 60);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, "Continuar");
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, victron_warning_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void victron_warning_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    if (txt && strcmp(txt, "Cancelar") == 0) {
        /* Cerrar el msgbox y volver al menu principal */
        if (s_victron_warning) { lv_obj_del(s_victron_warning); s_victron_warning = NULL; }
        if (s_settings_menu && s_settings_main_page) {
            lv_menu_set_page(s_settings_menu, s_settings_main_page);
        }
    } else {
        if (s_victron_warning) { lv_obj_del(s_victron_warning); s_victron_warning = NULL; }
    }
}

/* Callback en el boton del menu principal para mostrar warning antes */
static void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron)
{
    style_settings_scrollbar(page_victron);
    /* Asignar evento al lv_obj para detectar cuando se carga */
    lv_obj_add_event_cb(page_victron, (lv_event_cb_t)NULL, LV_EVENT_SCREEN_LOADED, NULL);
    /* Root container — aprovecha todo el ancho del page */
    lv_obj_t *victron_container = lv_obj_create(page_victron);
    lv_obj_remove_style_all(victron_container);
    lv_obj_set_size(victron_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(victron_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(victron_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(victron_container, 16, 0);
    lv_obj_set_style_pad_gap(victron_container, 16, 0);
    lv_obj_set_scroll_dir(victron_container, LV_DIR_VER);

    /* === Card de controles (border rosa) — header + botones add/remove === */
    lv_obj_t *card_ctrl = ui_card_create(victron_container, UI_COLOR_RED);
    lv_obj_t *header = ui_card_set_title(card_ctrl, LV_SYMBOL_LIST,
                                         "Dispositivos Victron", UI_COLOR_RED);

    /* Botones +/- a la derecha del header */
    lv_obj_t *controls_row = lv_obj_create(header);
    lv_obj_remove_style_all(controls_row);
    lv_obj_set_size(controls_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(controls_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(controls_row, 10, 0);

    ui->victron_config.add_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.add_btn, 44, 44);
    lv_obj_set_style_bg_color(ui->victron_config.add_btn, UI_COLOR_GREEN, 0);
    lv_obj_set_style_radius(ui->victron_config.add_btn, 8, 0);
    lv_obj_t *lbl_add = lv_label_create(ui->victron_config.add_btn);
    lv_label_set_text(lbl_add, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_add);
    lv_obj_add_event_cb(ui->victron_config.add_btn,
                        victron_config_add_btn_event_cb, LV_EVENT_CLICKED, ui);

    ui->victron_config.remove_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.remove_btn, 44, 44);
    lv_obj_set_style_bg_color(ui->victron_config.remove_btn, UI_COLOR_RED_DARK, 0);
    lv_obj_set_style_radius(ui->victron_config.remove_btn, 8, 0);
    lv_obj_t *lbl_remove = lv_label_create(ui->victron_config.remove_btn);
    lv_label_set_text(lbl_remove, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_remove, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_remove);
    lv_obj_add_event_cb(ui->victron_config.remove_btn,
                        victron_config_remove_btn_event_cb, LV_EVENT_CLICKED, ui);

    /* Texto descriptivo dentro del card */
    lv_obj_t *lbl_header = lv_label_create(card_ctrl);
    lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_header, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_long_mode(lbl_header, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_header, lv_pct(100));
    lv_label_set_text(lbl_header, "Configura hasta 8 dispositivos Victron con su dirección MAC y clave AES.");

    ui->victron_config.container = victron_container;

    /* Lista vertical de cards de dispositivo */
    ui->victron_config.list = lv_obj_create(victron_container);
    lv_obj_remove_style_all(ui->victron_config.list);
    lv_obj_set_width(ui->victron_config.list, lv_pct(100));
    lv_obj_set_height(ui->victron_config.list, LV_SIZE_CONTENT);
    lv_obj_set_layout(ui->victron_config.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui->victron_config.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ui->victron_config.list, 14, 0);
    lv_obj_set_scroll_dir(ui->victron_config.list, LV_DIR_VER);

    /* Initialize victron config state */
    ui->victron_config.count = 0;
    ui->victron_config.updating = false;
    for (size_t i = 0; i < UI_MAX_VICTRON_DEVICES; ++i) {
        ui->victron_config.rows[i] = NULL;
        ui->victron_config.mac_textareas[i] = NULL;
        ui->victron_config.key_textareas[i] = NULL;
        ui->victron_config.name_textareas[i] = NULL;
        ui->victron_config.enabled_checkboxes[i] = NULL;
        ui->victron_config.device_type_labels[i] = NULL;
        ui->victron_config.product_name_labels[i] = NULL;
        ui->victron_config.error_labels[i] = NULL;
        ui->victron_config.status_containers[i] = NULL;
    }

    /* Load existing configuration */
    victron_config_load(ui);

    /* Update controls state */
    victron_config_update_controls(ui);
}

static void victron_config_load(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }

    victron_device_config_t devices[UI_MAX_VICTRON_DEVICES];
    uint8_t count = 0;
    
    esp_err_t err = load_victron_devices(devices, &count, UI_MAX_VICTRON_DEVICES);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "Failed to load Victron devices: %s", esp_err_to_name(err));
        count = 0;
    }

    ui->victron_config.count = count;
    
    /* Create UI rows for loaded devices */
    for (size_t i = 0; i < count; ++i) {
        victron_config_create_row(ui, i);
        
        /* Set the loaded values */
        if (ui->victron_config.mac_textareas[i]) {
            lv_textarea_set_text(ui->victron_config.mac_textareas[i], devices[i].mac_address);
        }
        
        if (ui->victron_config.name_textareas[i]) {
            lv_textarea_set_text(ui->victron_config.name_textareas[i], devices[i].device_name);
        }
        
        if (ui->victron_config.key_textareas[i]) {
            char hex_key[33] = {0};
            for (int j = 0; j < 16; ++j) {
                sprintf(hex_key + j * 2, "%02X", devices[i].aes_key[j]);
            }
            lv_textarea_set_text(ui->victron_config.key_textareas[i], hex_key);
        }
        
        if (ui->victron_config.enabled_checkboxes[i]) {
            if (devices[i].enabled) {
                lv_obj_add_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void victron_config_refresh(ui_state_t *ui)
{
    if (ui == NULL || ui->victron_config.list == NULL) {
        return;
    }

    ESP_LOGI(TAG_SETTINGS, "Refreshing Victron device configuration, free heap: %lu PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Clear existing device rows - borrar hijos del container uno a uno
    while(lv_obj_get_child_cnt(ui->victron_config.list) > 0) {
        lv_obj_del(lv_obj_get_child(ui->victron_config.list, 0));
    }
    for (size_t i = 0; i < UI_MAX_VICTRON_DEVICES; ++i) {
        ui->victron_config.rows[i] = NULL;
        ui->victron_config.mac_textareas[i] = NULL;
        ui->victron_config.key_textareas[i] = NULL;
        ui->victron_config.name_textareas[i] = NULL;
        ui->victron_config.enabled_checkboxes[i] = NULL;
        ui->victron_config.device_type_labels[i] = NULL;
        ui->victron_config.product_name_labels[i] = NULL;
        ui->victron_config.error_labels[i] = NULL;
        ui->victron_config.status_containers[i] = NULL;
    }

    // Reset count
    ui->victron_config.count = 0;

    // Reload configuration from storage
    victron_config_load(ui);

    // Update controls state
    victron_config_update_controls(ui);

    ESP_LOGI(TAG_SETTINGS, "Victron device refresh completed, showing %d devices", ui->victron_config.count);
}

static void victron_config_create_row(ui_state_t *ui, size_t index)
{
    if (ui == NULL || ui->victron_config.list == NULL || index >= UI_MAX_VICTRON_DEVICES) {
        return;
    }

    /* Card por dispositivo (border cyan, mismo estilo que las demás) */
    lv_obj_t *row = ui_card_create(ui->victron_config.list, UI_COLOR_CYAN);
    if (row == NULL) { ESP_LOGE("UI", "row create failed idx=%d", (int)index); return; }

    /* Header con título "Device N" + switch enabled a la derecha */
    char title_buf[20];
    snprintf(title_buf, sizeof(title_buf), "Device %d", (int)(index + 1));
    lv_obj_t *header_row = ui_card_set_title(row, LV_SYMBOL_BLUETOOTH,
                                             title_buf, UI_COLOR_CYAN);

    lv_obj_t *enabled_cb = lv_checkbox_create(header_row);
    lv_checkbox_set_text(enabled_cb, "Activo");
    lv_obj_set_style_text_font(enabled_cb, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(enabled_cb, UI_COLOR_TEXT, 0);
    lv_obj_add_event_cb(enabled_cb, victron_enabled_checkbox_event_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    /* Body en 2 columnas: izquierda inputs, derecha estado en vivo */
    lv_obj_t *body = lv_obj_create(row);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 14, 0);

    /* Columna izquierda — inputs (flex_grow=1 para mitad ancho) */
    lv_obj_t *col_left = lv_obj_create(body);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_height(col_left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col_left, 1);
    lv_obj_set_layout(col_left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col_left, 6, 0);

    lv_obj_t *name_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(name_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(name_label, "Nombre:");

    lv_obj_t *name_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(name_ta, 31);
    lv_obj_set_width(name_ta, lv_pct(100));
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_placeholder_text(name_ta, "ej. Solar Charger 1");
    lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    lv_obj_t *mac_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(mac_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(mac_label, "Dirección MAC:");

    lv_obj_t *mac_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(mac_ta, 17);
    lv_obj_set_width(mac_ta, lv_pct(100));
    lv_textarea_set_one_line(mac_ta, true);
    lv_textarea_set_placeholder_text(mac_ta, "XX:XX:XX:XX:XX:XX");
    lv_obj_set_style_text_font(mac_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    lv_obj_t *key_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(key_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(key_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(key_label, "Clave AES (32 hex):");

    lv_obj_t *key_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(key_ta, 32);
    lv_obj_set_width(key_ta, lv_pct(100));
    lv_textarea_set_one_line(key_ta, true);
    lv_textarea_set_placeholder_text(key_ta, "00000000000000000000000000000000");
    lv_obj_set_style_text_font(key_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    /* Columna derecha — sub-card de estado en vivo (border verde) */
    lv_obj_t *status_container = lv_obj_create(body);
    lv_obj_remove_style_all(status_container);
    lv_obj_set_height(status_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(status_container, 1);
    lv_obj_set_layout(status_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_container, 6, 0);
    lv_obj_set_style_pad_all(status_container, 12, 0);
    lv_obj_set_style_bg_color(status_container, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_opa(status_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_container, 8, 0);
    lv_obj_set_style_border_width(status_container, 1, 0);
    lv_obj_set_style_border_opa(status_container, LV_OPA_50, 0);
    lv_obj_set_style_border_color(status_container, UI_COLOR_GREEN, 0);

    lv_obj_t *device_type_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(device_type_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(device_type_lbl, "Tipo: --");
    lv_obj_set_style_text_color(device_type_lbl, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *product_name_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(product_name_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(product_name_lbl, "Producto: --");
    lv_obj_set_style_text_color(product_name_lbl, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *error_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(error_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(error_lbl, "Estado: esperando datos...");
    lv_obj_set_style_text_color(error_lbl, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_long_mode(error_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(error_lbl, lv_pct(100));

    /* Store references */
    ui->victron_config.rows[index] = row;
    ui->victron_config.mac_textareas[index] = mac_ta;
    ui->victron_config.key_textareas[index] = key_ta;
    ui->victron_config.name_textareas[index] = name_ta;
    ui->victron_config.enabled_checkboxes[index] = enabled_cb;
    ui->victron_config.device_type_labels[index] = device_type_lbl;
    ui->victron_config.product_name_labels[index] = product_name_lbl;
    ui->victron_config.error_labels[index] = error_lbl;
    ui->victron_config.status_containers[index] = status_container;
}

static void victron_config_update_controls(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }

    bool can_add = ui->victron_config.count < UI_MAX_VICTRON_DEVICES;
    bool can_remove = ui->victron_config.count > 0;

    if (ui->victron_config.add_btn != NULL) {
        if (can_add) {
            lv_obj_clear_state(ui->victron_config.add_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ui->victron_config.add_btn, LV_STATE_DISABLED);
        }
    }

    if (ui->victron_config.remove_btn != NULL) {
        if (can_remove) {
            lv_obj_clear_state(ui->victron_config.remove_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ui->victron_config.remove_btn, LV_STATE_DISABLED);
        }
    }
}

static void victron_config_persist(ui_state_t *ui)
{
    if (ui == NULL || ui->victron_config.updating) {
        return;
    }

    victron_device_config_t devices[UI_MAX_VICTRON_DEVICES];
    memset(devices, 0, sizeof(devices));

    /* Collect data from UI */
    for (size_t i = 0; i < ui->victron_config.count; ++i) {
        /* Device name */
        if (ui->victron_config.name_textareas[i]) {
            const char *name = lv_textarea_get_text(ui->victron_config.name_textareas[i]);
            if (name && name[0] != '\0') {
                strncpy(devices[i].device_name, name, sizeof(devices[i].device_name) - 1);
            }
        }

        /* MAC address */
        if (ui->victron_config.mac_textareas[i]) {
            const char *mac = lv_textarea_get_text(ui->victron_config.mac_textareas[i]);
            if (mac && strlen(mac) == 17) {
                strncpy(devices[i].mac_address, mac, sizeof(devices[i].mac_address) - 1);
            } else {
                strcpy(devices[i].mac_address, "00:00:00:00:00:00");
            }
        }

        /* AES Key */
        if (ui->victron_config.key_textareas[i]) {
            const char *hex = lv_textarea_get_text(ui->victron_config.key_textareas[i]);
            if (hex && strlen(hex) == 32) {
                for (int j = 0; j < 16; ++j) {
                    char tmp[3] = { hex[j * 2], hex[j * 2 + 1], 0 };
                    devices[i].aes_key[j] = (uint8_t)strtol(tmp, NULL, 16);
                }
            }
        }

        /* Enabled state */
        if (ui->victron_config.enabled_checkboxes[i]) {
            devices[i].enabled = lv_obj_has_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
        }
    }

    esp_err_t err = save_victron_devices(devices, ui->victron_config.count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "Failed to save Victron devices: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_SETTINGS, "Saved %d Victron devices", ui->victron_config.count);
        // Reload BLE configuration to use updated device settings
        victron_ble_reload_device_config();
    }
}

void ui_settings_panel_init(ui_state_t *ui,
                            const char *default_ssid,
                            const char *default_pass,
                            uint8_t ap_enabled)
{
    if (ui == NULL || ui->tab_settings == NULL) {
        return;
    }

    lv_obj_t *menu = lv_menu_create(ui->tab_settings);
    lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
    lv_obj_center(menu);
    /* Fondo coherente con el resto de pestanas */
    lv_obj_set_style_bg_color(menu, lv_color_black(), 0);
    lv_obj_set_style_bg_color(lv_menu_get_main_header(menu), lv_color_black(), 0);
    lv_obj_set_style_text_color(lv_menu_get_main_header(menu), lv_color_white(), 0);
    ui->settings_menu = menu;
    s_settings_menu = menu;  /* referencia static para diálogos modales */

    lv_obj_t *main_header = lv_menu_get_main_header(menu);
    lv_obj_set_style_text_font(main_header, &lv_font_montserrat_28_es, 0);
    lv_obj_set_flex_align(main_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back_btn = lv_menu_get_main_header_back_btn(menu);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    /* Botón naranja cálido con sombra naranja exterior */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(back_btn, 2, 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_set_style_pad_hor(back_btn, 18, 0);
    lv_obj_set_style_pad_ver(back_btn, 10, 0);
    lv_obj_set_style_shadow_width(back_btn, 12, 0);
    lv_obj_set_style_shadow_color(back_btn, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_shadow_opa(back_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(back_btn, 0, 0);
    /* Estado pulsado: naranja oscuro */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE65100), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(back_btn, lv_color_white(), 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    /* Spacer invisible para centrar el titulo via SPACE_BETWEEN */
    lv_obj_t *header_spacer = lv_obj_create(main_header);
    lv_obj_remove_style_all(header_spacer);
    lv_obj_set_size(header_spacer, 110, 1);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    s_settings_main_page = main_page;
    lv_obj_t *page_frigo = lv_menu_page_create(menu, "FRIGO");
    ui->frigo_page = page_frigo;
    lv_obj_t *page_logs = lv_menu_page_create(menu, "LOGS");
    lv_obj_t *page_sound = lv_menu_page_create(menu, "SONIDO Y AVISOS");
    lv_obj_t *page_wifi = lv_menu_page_create(menu, "WI-FI");

    lv_obj_t *page_display = lv_menu_page_create(menu, "DISPLAY");
    lv_obj_t *page_victron = lv_menu_page_create(menu, "VICTRON KEYS");
    lv_obj_t *page_about = lv_menu_page_create(menu, LV_SYMBOL_LIST "  ABOUT VictronSolarDisplay");
    
    /* Padding del main_page + layout 2 columnas */
    lv_obj_set_style_pad_all(main_page, 16, 0);
    lv_obj_set_style_pad_row(main_page, 12, 0);
    lv_obj_set_style_pad_column(main_page, 12, 0);
    lv_obj_set_layout(main_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    settings_menu_add_entry(ui, main_page, menu, page_frigo,   "Frigo");
    settings_menu_add_entry(ui, main_page, menu, page_logs,    "Logs");
    settings_menu_add_entry(ui, main_page, menu, page_wifi,    "Wi-Fi");
    settings_menu_add_entry(ui, main_page, menu, page_display, "Display");
    settings_menu_add_entry(ui, main_page, menu, page_sound,   "Sonido y avisos");
    settings_menu_add_entry(ui, main_page, menu, page_victron, "Victron Keys");
    /* Mostrar warning al entrar en Victron Keys */
    {
        lv_obj_t *cont_vk = lv_obj_get_child(main_page, lv_obj_get_child_cnt(main_page) - 1);
        if (cont_vk) {
            lv_obj_add_event_cb(cont_vk, victron_keys_clicked_cb, LV_EVENT_CLICKED, ui);
        }
    }
    settings_menu_add_entry(ui, main_page, menu, page_about,   "About");

  
    lv_menu_set_page(menu, main_page);
    create_wifi_settings_page(ui, page_wifi, default_ssid, default_pass, ap_enabled);
    create_display_settings_page(ui, page_display);
    create_victron_keys_settings_page(ui, page_victron);
    create_logs_settings_page(ui, page_logs);
    create_sound_settings_page(ui, page_sound);
    create_about_settings_page(ui, page_about);
    ui_frigo_panel_init(ui);

    lv_obj_t *tab = ui->tab_settings;

    // remove default padding and layout effects
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_style_pad_row(tab, 0, 0);
    lv_obj_set_style_pad_column(tab, 0, 0);
    lv_obj_set_style_border_width(tab, 0, 0);

    // also ensure the menu expands fully
    lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
    lv_obj_align(menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

}

void ui_settings_panel_on_user_activity(ui_state_t *ui)
{
    screensaver_wake(ui);
}

void ui_settings_panel_set_mac(ui_state_t *ui, const char *mac_str)
{
    if (ui == NULL || ui->ta_mac == NULL || mac_str == NULL) {
        return;
    }
    lv_textarea_set_text(ui->ta_mac, mac_str);
}

void ui_settings_panel_update_victron_device_status(ui_state_t *ui, const char *mac_address, 
                                                     const char *device_type, const char *product_name, 
                                                     const char *error_info)
{
    if (ui == NULL) {
        return;
    }
    victron_config_update_device_status(ui, mac_address, device_type, product_name, error_info);
}

static void ta_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (ui == NULL || ui->keyboard == NULL) {
        return;
    }

    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(ui->keyboard, ta);
        lv_obj_move_foreground(ui->keyboard);
        lv_obj_clear_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_coord_t kb_height = lv_obj_get_height(ui->keyboard);
        lv_coord_t available_h = screen_h - kb_height;
        if (available_h < screen_h / 3) {
            available_h = screen_h / 3;
        }
        lv_obj_update_layout(ui->tabview);
        lv_obj_set_height(ui->tabview, available_h);
        lv_obj_update_layout(ui->tabview);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        if (ta == NULL) {
            return;
        }
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_keyboard_set_textarea(ui->keyboard, NULL);
        lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_obj_set_height(ui->tabview, screen_h);
        lv_obj_update_layout(ui->tabview);
        lv_indev_reset(NULL, ta);
    }
}

static void wifi_event_cb(lv_event_t *e)
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
            nvs_set_str(h, "ssid", txt);
        } else if (ta == ui->wifi.password) {
            nvs_set_str(h, "password", txt);
        }
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG_SETTINGS, "Wi-Fi config saved");
    } else {
        ESP_LOGE(TAG_SETTINGS, "nvs_open failed: %s", esp_err_to_name(err));
    }
}

static void password_toggle_btn_event_cb(lv_event_t *e)
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
    ui->brightness = (uint8_t)val;
    save_brightness(ui->brightness);
    apply_brightness_for_now(ui);   /* respeta noche si estamos en franja */
    /* Update label */
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", val);
    ESP_LOGI(TAG_SETTINGS, "Brightness set to %d", val);
    /* Ajustar máximo del slider screensaver al nuevo brightness */
    if (ui->screensaver.slider_brightness != NULL) {
        int ss_val = lv_slider_get_value(ui->screensaver.slider_brightness);
        lv_slider_set_range(ui->screensaver.slider_brightness, 0, val);
        /* Si el valor actual supera el nuevo máximo, lo recortamos */
        if (ss_val > val) {
            lv_slider_set_value(ui->screensaver.slider_brightness, val, LV_ANIM_OFF);
            ui->screensaver.brightness = (uint8_t)val;
            save_screensaver_settings(ui->screensaver.enabled, ui->screensaver.brightness, ui->screensaver.timeout);
        }
    }
}

/* ── Callbacks Modo nocturno ───────────────────────────────────── */
static void night_save_and_apply(ui_state_t *ui)
{
    save_night_mode(ui->night_mode.enabled,
                    ui->night_mode.start_h,
                    ui->night_mode.end_h,
                    ui->night_mode.brightness);
    apply_brightness_for_now(ui);
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

static void night_brightness_slider_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui) return;
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    int snapped = ((val + 2) / 5) * 5;
    if (snapped < 5) snapped = 5;
    if (snapped > 100) snapped = 100;
    if (snapped != val) {
        lv_slider_set_value(slider, snapped, LV_ANIM_OFF);
        val = snapped;
    }
    ui->night_mode.brightness = (uint8_t)val;
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", val);
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
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(ui->screensaver.slider_brightness);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", v);
    if (ui->screensaver.active) {
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
    }
}

/* Aplica el cambio de timeout: persiste en NVS y reprograma el timer */
static void ss_timeout_apply(ui_state_t *ui)
{
    if (ui->screensaver.spinbox_timeout) {
        lv_label_set_text_fmt(ui->screensaver.spinbox_timeout,
                              "%d", ui->screensaver.timeout / 60);
    }
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    if (ui->screensaver.timer) {
        uint32_t ms = ui->screensaver.timeout > 0
                          ? ui->screensaver.timeout * 1000U
                          : 0xFFFFFFFF;
        lv_timer_set_period(ui->screensaver.timer, ms);
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
    if (min > 0) min--;
    ui->screensaver.timeout = min * 60;
    ss_timeout_apply(ui);
}

void ui_settings_screensaver_create_timer(ui_state_t *ui)
{
    if (!ui || ui->screensaver.timer) return;
    /* Crear timer pausado, screensaver_enable lo activara */
    ui->screensaver.timer = lv_timer_create(screensaver_timer_cb,
                                             ui->screensaver.timeout * 1000U, ui);
    lv_timer_pause(ui->screensaver.timer);
    if (ui->screensaver.enabled) {
        screensaver_enable(ui, true);
    }
}

static void screensaver_enable(ui_state_t *ui, bool enable)
{
    if (ui == NULL || ui->screensaver.timer == NULL) {
        return;
    }
    if (enable) {
        ui->screensaver.active = false;
        bsp_display_brightness_set(ui->brightness);
        lv_timer_set_period(ui->screensaver.timer, ui->screensaver.timeout * 1000U);
        lv_timer_reset(ui->screensaver.timer);
        lv_timer_resume(ui->screensaver.timer);
    } else {
        lv_timer_pause(ui->screensaver.timer);
        if (ui->screensaver.active) {
            bsp_display_brightness_set(ui->brightness);
            ui->screensaver.active = false;
        }
    }
}

/* Forward declarations para rotacion */
extern void ui_show_battery_history_screen(ui_state_t *ui);
extern void ui_show_chart_screen(ui_state_t *ui);

extern void ui_close_chart_screen(void);
extern void ui_close_battery_history_screen(void);

static void screensaver_rotate_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : NULL;
    if (!ui) return;
    int prev_idx = ui->screensaver.rotate_index;
    int next_idx = (prev_idx + 1) % 3;
    ESP_LOGI("SAVER", "rotate fired idx=%d->%d", prev_idx, next_idx);

    /* 3 vistas: 0=Live (tab del tabview), 1=LogFrigo (overlay chart),
     * 2=LogBateria (pantalla independiente).
     * Cerrar SOLO la vista actual antes de abrir la siguiente, para que la
     * transición sea limpia (Frigo→Bateria cierra Frigo; Bateria→Live cierra Bateria). */
    switch (prev_idx) {
        case 1:
            ui_close_chart_screen();
            break;
        case 2:
            ui_close_battery_history_screen();
            break;
        default:
            break;  /* 0 = Live: no hay overlay que cerrar */
    }

    ui->screensaver.rotate_index = next_idx;
    switch (next_idx) {
        case 0:
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            break;
        case 1:
            ui_show_chart_screen(ui);
            break;
        case 2:
            ui_show_battery_history_screen(ui);
            break;
    }
}

static void screensaver_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : NULL;
    if (ui == NULL) {
        return;
    }
    if (!ui->screensaver.enabled || ui->screensaver.active) return;

    ESP_LOGI("SAVER", "timer fired mode=%d period=%d", ui->screensaver.mode, ui->screensaver.rotate_period_min);

    if (ui->screensaver.mode == UI_SCREENSAVER_MODE_ROTATE) {
        /* Modo rotar: brillo bajo + iniciar rotacion */
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
        ui->screensaver.active = true;
        ui->screensaver.rotate_index = 0;
        /* Crear timer de rotacion si no existe */
        if (ui->screensaver.rotate_timer) {
            lv_timer_del(ui->screensaver.rotate_timer);
        }
        uint32_t period_ms = (uint32_t)ui->screensaver.rotate_period_min * 60U * 1000U;
        ui->screensaver.rotate_timer = lv_timer_create(screensaver_rotate_timer_cb, period_ms, ui);
    } else {
        /* Modo atenuar (default) */
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
        ui->screensaver.active = true;
    }
}

static void screensaver_wake(ui_state_t *ui)
{
    if (ui == NULL || ui->screensaver.timer == NULL) {
        return;
    }
    if (ui->screensaver.enabled) {
        lv_timer_reset(ui->screensaver.timer);
        if (ui->screensaver.active) {
            bool was_rotating = (ui->screensaver.rotate_timer != NULL);
            bsp_display_brightness_set(ui->brightness);
            ui->screensaver.active = false;
            /* Parar timer de rotacion si estaba activo */
            if (ui->screensaver.rotate_timer) {
                lv_timer_del(ui->screensaver.rotate_timer);
                ui->screensaver.rotate_timer = NULL;
            }
            /* En modo rotación: al despertar, cerrar TODOS los overlays
             * (chart frigo, histórico batería) y volver a la pestaña Live. */
            if (was_rotating) {
                ui_close_chart_screen();
                ui_close_battery_history_screen();
                if (ui->tabview &&
                    lv_tabview_get_tab_act(ui->tabview) != 0) {
                    lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
                }
            }
            ui->screensaver.rotate_index = 0;
        }
    }
}



/* ── Modal de confirmación reutilizable para acciones en Victron Keys ──
 * Cualquier modificación (añadir, quitar, toggle activo) pasa por aquí. */
typedef void (*victron_confirm_fn)(void *ud);

static lv_obj_t *s_victron_confirm_modal = NULL;
static victron_confirm_fn s_victron_confirm_ok = NULL;
static victron_confirm_fn s_victron_confirm_cancel = NULL;
static void *s_victron_confirm_ud = NULL;

static void victron_confirm_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    bool confirmed = (txt && strstr(txt, "Confirmar") != NULL);

    victron_confirm_fn ok = s_victron_confirm_ok;
    victron_confirm_fn cancel = s_victron_confirm_cancel;
    void *ud = s_victron_confirm_ud;
    s_victron_confirm_ok = NULL;
    s_victron_confirm_cancel = NULL;
    s_victron_confirm_ud = NULL;

    if (s_victron_confirm_modal) {
        lv_obj_del(s_victron_confirm_modal);
        s_victron_confirm_modal = NULL;
    }
    if (confirmed) { if (ok) ok(ud); }
    else           { if (cancel) cancel(ud); }
}

static void victron_show_confirm_modal(const char *msg,
                                       victron_confirm_fn on_ok,
                                       victron_confirm_fn on_cancel,
                                       void *ud)
{
    if (s_victron_confirm_modal) return;
    s_victron_confirm_ok = on_ok;
    s_victron_confirm_cancel = on_cancel;
    s_victron_confirm_ud = ud;

    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_victron_confirm_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 560, 240);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  ¿Confirmar cambio?");

    lv_obj_t *m = lv_label_create(dlg);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(m, lv_color_white(), 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(m, msg ? msg : "Vas a modificar la configuración. ¿Continuar?");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 200, 56);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, victron_confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 200, 56);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, "Confirmar");
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, victron_confirm_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Acciones reales tras confirmación ─────────────────────────────── */

static void victron_do_add(void *ud)
{
    ui_state_t *ui = (ui_state_t *)ud;
    if (!ui || ui->victron_config.count >= UI_MAX_VICTRON_DEVICES) return;
    size_t index = ui->victron_config.count;
    ui->victron_config.count++;
    victron_config_create_row(ui, index);
    victron_config_update_controls(ui);
    victron_config_persist(ui);
}

static void victron_do_remove(void *ud)
{
    ui_state_t *ui = (ui_state_t *)ud;
    if (!ui || ui->victron_config.count == 0) return;
    size_t index = ui->victron_config.count - 1;
    if (ui->victron_config.rows[index] != NULL) {
        lv_obj_del(ui->victron_config.rows[index]);
    }
    ui->victron_config.rows[index] = NULL;
    ui->victron_config.mac_textareas[index] = NULL;
    ui->victron_config.key_textareas[index] = NULL;
    ui->victron_config.name_textareas[index] = NULL;
    ui->victron_config.enabled_checkboxes[index] = NULL;
    ui->victron_config.device_type_labels[index] = NULL;
    ui->victron_config.product_name_labels[index] = NULL;
    ui->victron_config.error_labels[index] = NULL;
    ui->victron_config.status_containers[index] = NULL;
    ui->victron_config.count--;
    victron_config_update_controls(ui);
    victron_config_persist(ui);
}

/* Para el toggle: guardamos el checkbox + nuevo estado para revertir */
typedef struct {
    ui_state_t *ui;
    lv_obj_t *cb;
    bool new_state;  /* estado al que se cambió antes de mostrar el modal */
} victron_toggle_ctx_t;

static victron_toggle_ctx_t s_toggle_ctx;

static void victron_do_toggle_confirm(void *ud)
{
    victron_toggle_ctx_t *ctx = (victron_toggle_ctx_t *)ud;
    if (!ctx || !ctx->ui) return;
    victron_config_persist(ctx->ui);
}

static void victron_do_toggle_cancel(void *ud)
{
    victron_toggle_ctx_t *ctx = (victron_toggle_ctx_t *)ud;
    if (!ctx || !ctx->cb) return;
    /* Revertir el estado del checkbox al anterior (opuesto al nuevo) */
    if (ctx->new_state) lv_obj_clear_state(ctx->cb, LV_STATE_CHECKED);
    else                lv_obj_add_state(ctx->cb, LV_STATE_CHECKED);
}

/* ── Wrappers de los event_cb originales con confirmación ──────────── */

static void victron_config_add_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui || ui->victron_config.count >= UI_MAX_VICTRON_DEVICES) return;
    victron_show_confirm_modal("Vas a añadir un nuevo dispositivo Victron. ¿Continuar?",
                               victron_do_add, NULL, ui);
}

static void victron_config_remove_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui || ui->victron_config.count == 0) return;
    victron_show_confirm_modal("Vas a eliminar el último dispositivo Victron. ¿Continuar?",
                               victron_do_remove, NULL, ui);
}

static void victron_enabled_checkbox_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    if (!ui || !cb || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Capturar nuevo estado para poder revertir si cancela */
    s_toggle_ctx.ui = ui;
    s_toggle_ctx.cb = cb;
    s_toggle_ctx.new_state = lv_obj_has_state(cb, LV_STATE_CHECKED);
    const char *msg = s_toggle_ctx.new_state
        ? "Vas a activar este dispositivo. ¿Continuar?"
        : "Vas a desactivar este dispositivo. ¿Continuar?";
    victron_show_confirm_modal(msg,
        victron_do_toggle_confirm, victron_do_toggle_cancel, &s_toggle_ctx);
}

static void victron_field_ta_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);
    if (!(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY)) {
        return;
    }

    victron_config_persist(ui);
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

static int victron_config_find_device_by_mac(ui_state_t *ui, const char *mac_address)
{
    if (ui == NULL || mac_address == NULL) {
        return -1;
    }

    for (size_t i = 0; i < ui->victron_config.count; ++i) {
        if (ui->victron_config.mac_textareas[i] != NULL) {
            const char *configured_mac = lv_textarea_get_text(ui->victron_config.mac_textareas[i]);
            if (configured_mac != NULL && strcmp(configured_mac, mac_address) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void victron_config_update_device_status(ui_state_t *ui, const char *mac_address, 
                                                const char *device_type, const char *product_name, 
                                                const char *error_info)
{
    if (ui == NULL) {
        return;
    }

    int device_index = victron_config_find_device_by_mac(ui, mac_address);
    if (device_index < 0) {
        return;  // Device not found in configuration
    }

    size_t index = (size_t)device_index;

    /* Update device type */
    if (ui->victron_config.device_type_labels[index]) {
        if (device_type && device_type[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.device_type_labels[index], "Device: %s", device_type);
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], lv_color_hex(0x00C851), 0); // Green for active
        } else {
            lv_label_set_text(ui->victron_config.device_type_labels[index], "Device: --");
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], lv_color_hex(0x888888), 0); // Gray for inactive
        }
    }

    /* Update product name */
    if (ui->victron_config.product_name_labels[index]) {
        if (product_name && product_name[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.product_name_labels[index], "Product: %s", product_name);
            lv_obj_set_style_text_color(ui->victron_config.product_name_labels[index], lv_color_hex(0x00C851), 0); // Green for active
        } else {
            lv_label_set_text(ui->victron_config.product_name_labels[index], "Product: --");
            lv_obj_set_style_text_color(ui->victron_config.product_name_labels[index], lv_color_hex(0x888888), 0); // Gray for inactive
        }
    }

    /* Update error/status info */
    if (ui->victron_config.error_labels[index]) {
        if (error_info && error_info[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.error_labels[index], "Status: %s", error_info);
            /* Color code based on content */
            if (strstr(error_info, "error") || strstr(error_info, "Error") || strstr(error_info, "ERROR")) {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0xF44336), 0); // Red for errors
            } else if (strstr(error_info, "Active") || strstr(error_info, "OK") || strstr(error_info, "Connected")) {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0x00C851), 0); // Green for OK
            } else {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0xFF9800), 0); // Orange for warnings
            }
        } else {
            lv_label_set_text(ui->victron_config.error_labels[index], "Status: No data");
            lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0x888888), 0); // Gray for no data
        }
    }
}

void ui_settings_panel_refresh_victron_devices(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }
    
    ESP_LOGI(TAG_SETTINGS, "Public function called to refresh Victron device list");
    victron_config_refresh(ui);
}

/* ── Trip computer: refresco periodico y reset ─────────────────── */
static lv_obj_t *s_trip_label = NULL;

static void trip_label_refresh(void)
{
    if (!s_trip_label) return;
    trip_computer_t t;
    trip_computer_get(&t);
    char start_str[24] = "--";
    if (t.reset_epoch > 0) {
        struct tm tm_l;
        localtime_r((time_t *)&t.reset_epoch, &tm_l);
        strftime(start_str, sizeof(start_str), "%d/%m %H:%M", &tm_l);
    }
    int hours = (int)(t.seconds_running / 3600);
    int minutes = (int)((t.seconds_running % 3600) / 60);
    char buf[160];
    snprintf(buf, sizeof(buf),
        "Desde %s   |   %dh %02dm activo\n"
        "Cargado: %.2f kWh  (%.1f Ah)\n"
        "Consumido: %.2f kWh  (%.1f Ah)",
        start_str, hours, minutes,
        t.wh_charged / 1000.0, t.ah_charged,
        t.wh_discharged / 1000.0, t.ah_discharged);
    lv_label_set_text(s_trip_label, buf);
}

static void trip_reset_msgbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    if (btn_id == 0) {  /* "Si" */
        trip_computer_reset();
        trip_label_refresh();
    }
    lv_msgbox_close(mbox);
}

static void trip_reset_btn_cb(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = {"Si", "Cancelar", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Trip computer",
        "Resetear contadores del viaje? Esta accion no se puede deshacer.",
        btns, false);
    lv_obj_add_event_cb(mbox, trip_reset_msgbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

/* ── Callbacks Backup/Restore configuración ───────────────────── */
static void backup_export_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *status = (lv_obj_t *)lv_obj_get_user_data(btn);
    esp_err_t err = config_backup_export(CONFIG_BACKUP_PATH);
    if (status) {
        lv_label_set_text(status, err == ESP_OK
            ? "Exportado a " CONFIG_BACKUP_PATH
            : "ERROR exportando (¿SD montada?)");
        lv_obj_set_style_text_color(status,
            err == ESP_OK ? lv_color_hex(0x00C851) : lv_color_hex(0xCC3333), 0);
    }
}

static void backup_import_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *status = (lv_obj_t *)lv_obj_get_user_data(btn);
    esp_err_t err = config_backup_import(CONFIG_BACKUP_PATH);
    if (status) {
        lv_label_set_text(status, err == ESP_OK
            ? "Importado. Reinicia para aplicar todos los cambios."
            : "ERROR importando (¿fichero existe?)");
        lv_obj_set_style_text_color(status,
            err == ESP_OK ? lv_color_hex(0x00C851) : lv_color_hex(0xCC3333), 0);
    }
}

static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    style_settings_scrollbar(page);
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(cont, 16, 0);
    lv_obj_set_style_pad_all(cont, 16, 0);


    /* === Card 2: Info dinamica === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 10, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_REFRESH "  Estado");

    ui->lbl_about_uptime = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_uptime, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_uptime, "Uptime: --");

    /* Fila RAM + SD */
    lv_obj_t *row_mem = lv_obj_create(card2);
    lv_obj_remove_style_all(row_mem);
    lv_obj_set_width(row_mem, lv_pct(100));
    lv_obj_set_height(row_mem, LV_SIZE_CONTENT);
    lv_obj_set_layout(row_mem, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_mem, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mem, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui->lbl_about_heap = lv_label_create(row_mem);
    lv_obj_set_style_text_font(ui->lbl_about_heap, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_heap, "RAM libre: --");

    ui->lbl_about_sd = lv_label_create(row_mem);
    lv_obj_set_style_text_font(ui->lbl_about_sd, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_align(ui->lbl_about_sd, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(ui->lbl_about_sd, "SD: --");

    ui->lbl_about_ip = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_ip, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_ip, "IP AP: --");

    /* Diagnostico de salud: causa del ultimo reset + total de resets WDT/panic */
    lv_obj_t *lbl_wd = lv_label_create(card2);
    lv_obj_set_style_text_font(lbl_wd, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_wd, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text_fmt(lbl_wd, "Ultimo reset: %s   |   Resets WDT/panic: %lu",
                          watchdog_last_reset_reason(),
                          (unsigned long)watchdog_get_reset_count());

    /* === Card 3: Credits === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 6, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_COLUMN);

    /* Header row: titulo a la izquierda, boton reiniciar a la derecha */
    lv_obj_t *card3_header = lv_obj_create(card3);
    lv_obj_remove_style_all(card3_header);
    lv_obj_set_size(card3_header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card3_header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3_header);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_LIST "  Version, Repo y Creditos");

    /* Boton Reiniciar pequeno en la esquina */
    lv_obj_t *btn_reboot_hdr = lv_btn_create(card3_header);
    lv_obj_set_size(btn_reboot_hdr, 130, 40);
    lv_obj_set_style_bg_color(btn_reboot_hdr, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_reboot_hdr, 8, 0);
    lv_obj_t *lbl_reboot_hdr = lv_label_create(btn_reboot_hdr);
    lv_label_set_text(lbl_reboot_hdr, LV_SYMBOL_POWER "  Reiniciar");
    lv_obj_set_style_text_font(lbl_reboot_hdr, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_reboot_hdr);
    lv_obj_add_event_cb(btn_reboot_hdr, reboot_btn_cb, LV_EVENT_CLICKED, ui);

    /* Version */
    lv_obj_t *lbl_ver_top = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_ver_top, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_ver_top, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text_fmt(lbl_ver_top, "Version: %s", APP_VERSION);

    /* Chip + IDF */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    lv_obj_t *lbl_chip = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_chip, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_chip, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text_fmt(lbl_chip, "ESP32 model=%d cores=%d rev=%d  |  IDF: %s",
        chip.model, chip.cores, chip.revision, esp_get_idf_version());

    lv_obj_t *lbl_port = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_port, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_port, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(lbl_port, "Port para Guition JC1060P470C_I por Ehuntabi");

    lv_obj_t *lbl_gh = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_gh, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_gh, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(lbl_gh, "github.com/Ehuntabi/victron-jc1060p470c-esp32p4");

    lv_obj_t *lbl_cred = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_cred, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_cred, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_cred, "Basado en: CamdenSutherland, wytr");

    /* === Card Trip computer (contadores reseteables del viaje) === */
    lv_obj_t *card_trip = lv_obj_create(cont);
    lv_obj_set_width(card_trip, lv_pct(100));
    lv_obj_set_height(card_trip, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_trip, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_trip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_trip, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(card_trip, 1, 0);
    lv_obj_set_style_radius(card_trip, 12, 0);
    lv_obj_set_style_pad_all(card_trip, 16, 0);
    lv_obj_set_style_pad_gap(card_trip, 10, 0);
    lv_obj_set_layout(card_trip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_trip, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *trip_head = lv_obj_create(card_trip);
    lv_obj_remove_style_all(trip_head);
    lv_obj_set_size(trip_head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_head, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trip_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *trip_title = lv_label_create(trip_head);
    lv_obj_set_style_text_font(trip_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(trip_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(trip_title, LV_SYMBOL_REFRESH "  Trip computer");

    lv_obj_t *btn_trip_rst = lv_btn_create(trip_head);
    lv_obj_set_size(btn_trip_rst, 140, 44);
    lv_obj_set_style_bg_color(btn_trip_rst, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_trip_rst, 8, 0);
    lv_obj_t *lbl_trip_rst = lv_label_create(btn_trip_rst);
    lv_label_set_text(lbl_trip_rst, "Reset");
    lv_obj_set_style_text_font(lbl_trip_rst, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_rst);
    lv_obj_add_event_cb(btn_trip_rst, trip_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    s_trip_label = lv_label_create(card_trip);
    lv_obj_set_style_text_font(s_trip_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_trip_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(s_trip_label, lv_pct(100));
    lv_label_set_long_mode(s_trip_label, LV_LABEL_LONG_WRAP);
    trip_label_refresh();

    /* === Card 4: Backup/Restore configuracion === */
    lv_obj_t *card_bak = lv_obj_create(cont);
    lv_obj_set_width(card_bak, lv_pct(100));
    lv_obj_set_height(card_bak, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_bak, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_bak, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_bak, lv_color_hex(0x9C27B0), 0);
    lv_obj_set_style_border_width(card_bak, 1, 0);
    lv_obj_set_style_radius(card_bak, 12, 0);
    lv_obj_set_style_pad_all(card_bak, 16, 0);
    lv_obj_set_style_pad_gap(card_bak, 12, 0);
    lv_obj_set_layout(card_bak, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_bak, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bak_title = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bak_title, lv_color_hex(0x9C27B0), 0);
    lv_label_set_text(bak_title, LV_SYMBOL_SD_CARD "  Backup configuracion");

    lv_obj_t *bak_desc = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_desc, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(bak_desc, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_width(bak_desc, lv_pct(100));
    lv_label_set_long_mode(bak_desc, LV_LABEL_LONG_WRAP);
    lv_label_set_text(bak_desc,
        "Exporta toda la configuracion (claves Victron, alertas, brillo, "
        "modo nocturno, screensaver, TZ) a /sdcard/config_backup.json. "
        "El password Wi-Fi no se exporta por seguridad.");

    lv_obj_t *bak_row = lv_obj_create(card_bak);
    lv_obj_remove_style_all(bak_row);
    lv_obj_set_size(bak_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(bak_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bak_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bak_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_exp = lv_btn_create(bak_row);
    lv_obj_set_size(btn_exp, 200, 50);
    lv_obj_set_style_bg_color(btn_exp, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn_exp, 10, 0);
    lv_obj_t *lbl_exp = lv_label_create(btn_exp);
    lv_label_set_text(lbl_exp, LV_SYMBOL_UPLOAD "  Exportar");
    lv_obj_set_style_text_font(lbl_exp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_exp);

    lv_obj_t *btn_imp = lv_btn_create(bak_row);
    lv_obj_set_size(btn_imp, 200, 50);
    lv_obj_set_style_bg_color(btn_imp, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(btn_imp, 10, 0);
    lv_obj_t *lbl_imp = lv_label_create(btn_imp);
    lv_label_set_text(lbl_imp, LV_SYMBOL_DOWNLOAD "  Importar");
    lv_obj_set_style_text_font(lbl_imp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_imp);

    /* Status label */
    lv_obj_t *bak_status = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_status, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(bak_status, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(bak_status, "");

    lv_obj_set_user_data(btn_exp, bak_status);
    lv_obj_set_user_data(btn_imp, bak_status);
    lv_obj_add_event_cb(btn_exp, backup_export_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(btn_imp, backup_import_cb, LV_EVENT_CLICKED, ui);

    /* Refrescar y crear timer */
    about_refresh_dynamic(ui);
    lv_timer_create(about_timer_cb, 1000, ui);

}

static void portal_page_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "portal_page", (uint8_t)sel);
        nvs_commit(h);
        nvs_close(h);
    }
    const char *name = sel == 0 ? "Keys" : (sel == 1 ? "Logs" : "Dashboard");
    ESP_LOGI(TAG_SETTINGS, "Portal page: %s", name);
}


static void settings_btn_styles_init(void)
{
    if (s_settings_styles_inited) return;
    lv_style_init(&s_settings_btn_style);
    lv_style_set_bg_opa(&s_settings_btn_style, LV_OPA_COVER);
    lv_style_set_bg_color(&s_settings_btn_style, lv_color_hex(0x2A2A2A));
    lv_style_set_border_color(&s_settings_btn_style, lv_color_hex(0x555555));
    lv_style_set_border_width(&s_settings_btn_style, 1);
    lv_style_set_radius(&s_settings_btn_style, 10);
    lv_style_set_pad_all(&s_settings_btn_style, 16);
    lv_style_set_min_height(&s_settings_btn_style, 70);

    lv_style_init(&s_settings_btn_pressed_style);
    lv_style_set_bg_color(&s_settings_btn_pressed_style, lv_color_hex(0x3D5A80));
    s_settings_styles_inited = true;
}

static void settings_menu_add_entry(ui_state_t *ui, lv_obj_t *main_page,
                                    lv_obj_t *menu, lv_obj_t *target_page,
                                    const char *text)
{
    settings_btn_styles_init();
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_style(cont, &s_settings_btn_style, 0);
    lv_obj_add_style(cont, &s_settings_btn_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_width(cont, lv_pct(48));
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Color por entrada */
    uint32_t bg = 0x2A4A6A;     /* fondo por defecto */
    uint32_t border = 0x4FC3F7; /* azul */
    const char *icon = LV_SYMBOL_LIST;
    if (strstr(text, "Frigo"))         { bg = 0x004D40; border = 0x00C851; icon = LV_SYMBOL_REFRESH; }
    else if (strstr(text, "Logs"))     { bg = 0x4A3300; border = 0xFFAA00; icon = LV_SYMBOL_SAVE; }
    else if (strstr(text, "Wi-Fi"))    { bg = 0x0D3B66; border = 0x4FC3F7; icon = LV_SYMBOL_WIFI; }
    else if (strstr(text, "Display"))  { bg = 0x2D004D; border = 0xBA68C8; icon = LV_SYMBOL_EYE_OPEN; }
    else if (strstr(text, "Sonido"))   { bg = 0x4A1A00; border = 0xFF7043; icon = LV_SYMBOL_VOLUME_MAX; }
    else if (strstr(text, "Victron"))  { bg = 0x4A0033; border = 0xE91E63; icon = LV_SYMBOL_GPS; }
    else if (strstr(text, "About"))    { bg = 0x1F3A4D; border = 0x90A4AE; icon = LV_SYMBOL_LIST; }
    lv_obj_set_style_bg_color(cont, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(cont, 2, 0);

    lv_obj_t *label = lv_label_create(cont);
    char full[40];
    snprintf(full, sizeof(full), "%s  %s", icon, text);
    lv_label_set_text(label, full);
    lv_obj_add_style(label, &ui->styles.medium, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_menu_set_load_page_event(menu, cont, target_page);
}


/* --- About page: info dinamica + reboot --- */
static void about_refresh_dynamic(ui_state_t *ui)
{
    if (!ui) return;
    /* Uptime */
    if (ui->lbl_about_uptime) {
        int64_t up_s = esp_timer_get_time() / 1000000;
        int d = up_s / 86400;
        int h = (up_s % 86400) / 3600;
        int m = (up_s % 3600) / 60;
        int s = up_s % 60;
        if (d > 0)
            lv_label_set_text_fmt(ui->lbl_about_uptime, "Uptime: %dd %02dh %02dm %02ds", d, h, m, s);
        else
            lv_label_set_text_fmt(ui->lbl_about_uptime, "Uptime: %02dh %02dm %02ds", h, m, s);
    }
    /* RAM libre */
    if (ui->lbl_about_heap) {
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_spi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        lv_label_set_text_fmt(ui->lbl_about_heap,
            "RAM libre: int %u KB  |  PSRAM %u KB",
            (unsigned)(free_int / 1024), (unsigned)(free_spi / 1024));
    }
    /* SD */
    if (ui->lbl_about_sd) {
        FATFS *fs = NULL;
        DWORD free_clusters = 0;
        if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
            uint64_t sect_per_cluster = fs->csize;
            uint64_t total_sect = (fs->n_fatent - 2) * sect_per_cluster;
            uint64_t free_sect  = (uint64_t)free_clusters * sect_per_cluster;
            uint64_t total_mb = (total_sect * 512ULL) / (1024ULL * 1024ULL);
            uint64_t free_mb  = (free_sect  * 512ULL) / (1024ULL * 1024ULL);
            lv_label_set_text_fmt(ui->lbl_about_sd, "SD: %u/%u MB libres",
                (unsigned)free_mb, (unsigned)total_mb);
        } else {
            lv_label_set_text(ui->lbl_about_sd, "SD: no montada");
        }
    }
    /* IP */
    if (ui->lbl_about_ip) {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info = {0};
        if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
            lv_label_set_text_fmt(ui->lbl_about_ip, "IP AP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            lv_label_set_text(ui->lbl_about_ip, "IP AP: --");
        }
    }
}

static void about_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    /* Skip si la pagina About no esta visible: refrescar uptime + RAM + SD
     * + IP cada segundo es costoso (heap_caps_get_free_size, statvfs SD)
     * y solo tiene sentido si el usuario esta mirando. */
    if (!ui || !ui->lbl_about_uptime ||
        !lv_obj_is_visible(ui->lbl_about_uptime)) {
        return;
    }
    about_refresh_dynamic(ui);
    trip_label_refresh();
}

static void reboot_msgbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    if (btn_id == 0) {
        ESP_LOGW(TAG_SETTINGS, "Reboot confirmed by user");
        lv_msgbox_close(mbox);
        /* Flush datos antes de reiniciar */
        ESP_LOGI("UI", "Flushing data before restart...");
        datalogger_flush();
        battery_history_flush();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        lv_msgbox_close(mbox);
    }
}

static void reboot_btn_cb(lv_event_t *e)
{
    static const char *btns[] = {"Si", "No", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Reiniciar",
        "Estas seguro de que quieres reiniciar el dispositivo?",
        btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, reboot_msgbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void logs_btn_bat_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_battery_history_screen(ui);
}

static void logs_btn_frigo_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_chart_screen(ui);
}

static void create_logs_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    style_settings_scrollbar(page);
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 24, 0);

    lv_obj_t *btn_frigo = lv_btn_create(cont);
    lv_obj_set_size(btn_frigo, lv_pct(50), 80);
    lv_obj_set_style_bg_color(btn_frigo, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_radius(btn_frigo, 10, 0);
    lv_obj_t *lbl_frigo = lv_label_create(btn_frigo);
    lv_label_set_text(lbl_frigo, "FRIGO");
    lv_obj_set_style_text_font(lbl_frigo, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_frigo, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_frigo);
    lv_obj_add_event_cb(btn_frigo, logs_btn_frigo_cb, LV_EVENT_CLICKED, ui);

    lv_obj_t *btn_bat = lv_btn_create(cont);
    lv_obj_set_size(btn_bat, lv_pct(50), 80);
    lv_obj_set_style_bg_color(btn_bat, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_radius(btn_bat, 10, 0);
    lv_obj_t *lbl_bat = lv_label_create(btn_bat);
    lv_label_set_text(lbl_bat, "BATERIA");
    lv_obj_set_style_text_font(lbl_bat, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_bat, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_bat);
    lv_obj_add_event_cb(btn_bat, logs_btn_bat_cb, LV_EVENT_CLICKED, ui);
}

/* === Pagina Sonido === */
static void sound_volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    /* Redondeo a multiplos de 5 */
    v = (v / 5) * 5;
    lv_slider_set_value(slider, v, LV_ANIM_OFF);
    audio_set_volume(v);
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "Volumen: %d%%", v);
}

static void sound_mute_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool muted = lv_obj_has_state(sw, LV_STATE_CHECKED);
    audio_set_mute(muted);
}


static void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;
    style_settings_scrollbar(page);
    /* Contenedor principal vertical */
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Sonido === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(100));
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

    lv_obj_t *card1_title = lv_label_create(card1);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_VOLUME_MAX "  Sonido");

    /* Volumen */
    lv_obj_t *lbl_vol = lv_label_create(card1);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20_es, 0);
    lv_label_set_text_fmt(lbl_vol, "Volumen: %d%%", audio_get_volume());

    lv_obj_t *slider = lv_slider_create(card1);
    lv_obj_set_width(slider, lv_pct(95));
    lv_obj_set_height(slider, 26);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4FC3F7), LV_PART_KNOB);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, sound_volume_changed_cb, LV_EVENT_VALUE_CHANGED, lbl_vol);

    /* Mute (texto + switch) */
    lv_obj_t *row_mute = lv_obj_create(card1);
    lv_obj_remove_style_all(row_mute);
    lv_obj_set_size(row_mute, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_mute, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_mute, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mute, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_mute, 16, 0);

    lv_obj_t *lbl_mute = lv_label_create(row_mute);
    lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_mute, "Silenciar avisos");

    lv_obj_t *sw = lv_switch_create(row_mute);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4FC3F7), LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (audio_is_muted()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sound_mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui->sound_mute_switch = sw;

    /* === Card 2: Bateria === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 16, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_BATTERY_FULL "  Bateria");

    /* row_soc deja de ser child de card2; usa card2 como flex padre directamente */
    lv_obj_t *row_soc = card2;

    /* SoC Critico */
    lv_obj_t *col_crit = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_crit);
    lv_obj_set_layout(col_crit, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_crit, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_crit, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_crit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_crit, 6, 0);
    lv_obj_t *lbl_crit = lv_label_create(col_crit);
    lv_obj_set_style_text_font(lbl_crit, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_crit, lv_color_hex(0xFF4444), 0);
    lv_label_set_text(lbl_crit, LV_SYMBOL_WARNING " Critico");
    lv_obj_t *dd_crit = lv_dropdown_create(col_crit);
    lv_obj_set_width(dd_crit, 130);
    lv_dropdown_set_options(dd_crit, "10 %\n20 %\n30 %\n40 %");
    {
        int cur = alerts_get_soc_critical();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0]); ++k) {
            if (s_soc_crit_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_crit, idx);
    }
    lv_obj_add_event_cb(dd_crit, soc_crit_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* SoC Aviso */
    lv_obj_t *col_warn = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_warn);
    lv_obj_set_layout(col_warn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_warn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_warn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_warn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_warn, 6, 0);
    lv_obj_t *lbl_warn = lv_label_create(col_warn);
    lv_obj_set_style_text_font(lbl_warn, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_warn, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(lbl_warn, LV_SYMBOL_BELL " Aviso");
    lv_obj_t *dd_warn = lv_dropdown_create(col_warn);
    lv_obj_set_width(dd_warn, 130);
    lv_dropdown_set_options(dd_warn, "40 %\n50 %\n60 %\n70 %");
    {
        int cur = alerts_get_soc_warning();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0]); ++k) {
            if (s_soc_warn_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_warn, idx);
    }
    lv_obj_add_event_cb(dd_warn, soc_warn_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* === Card 3: Congelador === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 16, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_CHARGE "  Congelador");

    lv_obj_t *row_frigo = card3;

    /* Col minutos */
    lv_obj_t *col_min_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_min_a);
    lv_obj_set_layout(col_min_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_min_a, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_min_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_min_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_min_a, 6, 0);
    lv_obj_t *lbl_min_a = lv_label_create(col_min_a);
    lv_obj_set_style_text_font(lbl_min_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_min_a, "Tras subir (min)");
    lv_obj_t *dd_min_a = lv_dropdown_create(col_min_a);
    lv_obj_set_width(dd_min_a, 130);
    lv_dropdown_set_options(dd_min_a, "15\n30\n45\n60\n90");
    {
        static const int opts[] = { 15, 30, 45, 60, 90 };
        int cur = alerts_get_freezer_minutes();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_min_a, idx);
    }
    lv_obj_add_event_cb(dd_min_a, alarm_min_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);

    /* Col temp umbral */
    lv_obj_t *col_t_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_t_a);
    lv_obj_set_layout(col_t_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_t_a, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_t_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_t_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_t_a, 6, 0);
    lv_obj_t *lbl_t_a = lv_label_create(col_t_a);
    lv_obj_set_style_text_font(lbl_t_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_t_a, "Si supera");
    lv_obj_t *dd_t_a = lv_dropdown_create(col_t_a);
    lv_obj_set_width(dd_t_a, 140);
    lv_dropdown_set_options(dd_t_a, "-5 \xc2\xb0""C\n-2 \xc2\xb0""C\n0 \xc2\xb0""C\n+2 \xc2\xb0""C");
    {
        static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
        float cur = alerts_get_freezer_temp_c();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_t_a, idx);
    }
    lv_obj_add_event_cb(dd_t_a, alarm_temp_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);
}


static void alarm_min_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const int opts[] = { 15, 30, 45, 60, 90 };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_minutes(opts[sel]);
}

static void alarm_temp_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_temp_c(opts[sel]);
}
