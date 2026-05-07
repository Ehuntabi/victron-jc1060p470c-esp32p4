#include "settings_panel.h"
#include "ui.h"
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

// Forward declaration for view update function
extern void ui_force_view_update(void);

#define WIFI_NAMESPACE "wifi"

static const char *TAG_SETTINGS = "UI_SETTINGS";
static const char *APP_VERSION = "1.3.0";

static void ta_event_cb(lv_event_t *e);
static void wifi_event_cb(lv_event_t *e);
static void ap_checkbox_event_cb(lv_event_t *e);
static void password_toggle_btn_event_cb(lv_event_t *e);
static void reboot_btn_event_cb(lv_event_t *e);
static void about_refresh_dynamic(ui_state_t *ui);
static void about_timer_cb(lv_timer_t *t);
static void reboot_msgbox_cb(lv_event_t *e);
static void reboot_btn_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);
static void cb_screensaver_event_cb(lv_event_t *e);
static void victron_debug_event_cb(lv_event_t *e);
static void slider_ss_brightness_event_cb(lv_event_t *e);
static void spinbox_ss_time_event_cb(lv_event_t *e);
static void spinbox_ss_time_increment_event_cb(lv_event_t *e);
static void spinbox_ss_time_decrement_event_cb(lv_event_t *e);
static void screensaver_timer_cb(lv_timer_t *timer);
static void view_selection_dropdown_event_cb(lv_event_t *e);
static void screensaver_enable(ui_state_t *ui, bool enable);
static void screensaver_wake(ui_state_t *ui);


// Victron devices configuration functions
static void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron);
static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page_about);
static void create_logs_settings_page(ui_state_t *ui, lv_obj_t *page);

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
static lv_style_t s_settings_btn_style;
static lv_style_t s_settings_btn_pressed_style;
static bool s_settings_styles_inited = false;

static void settings_btn_styles_init(void);
static void settings_menu_add_entry(ui_state_t *ui, lv_obj_t *main_page,
                                    lv_obj_t *menu, lv_obj_t *target_page,
                                    const char *text);

static void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                                      const char *default_ssid,
                                      const char *default_pass,
                                      uint8_t ap_enabled)
{
    /* Root layout container */
    lv_obj_t *wifi_container = lv_obj_create(page_wifi);
    lv_obj_remove_style_all(wifi_container);
    lv_obj_set_size(wifi_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(wifi_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(wifi_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wifi_container, 10, 0);
    lv_obj_set_style_pad_gap(wifi_container, 12, 0);
    lv_obj_set_scroll_dir(wifi_container, LV_DIR_VER);

    /* SSID label */
    lv_obj_t *lbl_ssid = lv_label_create(wifi_container);
    lv_obj_add_style(lbl_ssid, &ui->styles.small, 0);
    lv_label_set_text(lbl_ssid, "SSID:");

    /* SSID row: input + checkbox */
    lv_obj_t *ssid_row = lv_obj_create(wifi_container);
    lv_obj_remove_style_all(ssid_row);
    lv_obj_set_width(ssid_row, lv_pct(100));
    lv_obj_set_height(ssid_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(ssid_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(ssid_row, 10, 0);
    lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* SSID input */
    ui->wifi.ssid = lv_textarea_create(ssid_row);
    lv_textarea_set_one_line(ui->wifi.ssid, true);
    lv_obj_set_width(ui->wifi.ssid, lv_pct(40));
    lv_textarea_set_text(ui->wifi.ssid, default_ssid);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, wifi_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->wifi.ssid, &ui->styles.small, 0);

    /* AP enable checkbox */

    /* Password label */
    lv_obj_t *lbl_pass = lv_label_create(wifi_container);
    lv_obj_add_style(lbl_pass, &ui->styles.small, 0);
    lv_label_set_text(lbl_pass, "Password:");

    const char *ap_password = (default_pass && default_pass[0] != '\0') ? default_pass : DEFAULT_AP_PASSWORD;

    /* Row for password + toggle button */
    lv_obj_t *pass_row = lv_obj_create(wifi_container);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_width(pass_row, lv_pct(100));
    lv_obj_set_height(pass_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(pass_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pass_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(pass_row, 10, 0);
    lv_obj_set_flex_align(pass_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Password textarea */
    ui->wifi.password = lv_textarea_create(pass_row);
    lv_textarea_set_password_mode(ui->wifi.password, true);
    lv_textarea_set_one_line(ui->wifi.password, true);
    lv_obj_set_width(ui->wifi.password, lv_pct(40));
    lv_textarea_set_text(ui->wifi.password, ap_password);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(ui->wifi.password, wifi_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->wifi.password, &ui->styles.small, 0);

    /* Show/Hide password toggle */
    ui->wifi.password_toggle = lv_btn_create(pass_row);
    lv_obj_set_width(ui->wifi.password_toggle, lv_pct(20));
    lv_obj_add_event_cb(ui->wifi.password_toggle, password_toggle_btn_event_cb, LV_EVENT_CLICKED, ui);

    lv_obj_t *lbl_toggle = lv_label_create(ui->wifi.password_toggle);
    lv_label_set_text(lbl_toggle, "Show");
    lv_obj_center(lbl_toggle);
    lv_obj_add_style(lbl_toggle, &ui->styles.small, 0);
    /* Separador */
    lv_obj_t *sep_portal = lv_obj_create(wifi_container);
    lv_obj_remove_style_all(sep_portal);
    lv_obj_set_width(sep_portal, lv_pct(100));
    lv_obj_set_height(sep_portal, 2);
    lv_obj_set_style_bg_color(sep_portal, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(sep_portal, LV_OPA_COVER, 0);
    lv_obj_t *lbl_portal = lv_label_create(wifi_container);
    lv_obj_add_style(lbl_portal, &ui->styles.small, 0);
    lv_label_set_text(lbl_portal, "Pagina inicial portal web:");
    lv_obj_t *dd_portal = lv_dropdown_create(wifi_container);
    lv_obj_add_style(dd_portal, &ui->styles.small, 0);
    lv_obj_set_width(dd_portal, lv_pct(60));
    lv_dropdown_set_options(dd_portal, "Keys\nLogs");
    lv_dropdown_set_selected(dd_portal, 0);
    lv_obj_add_event_cb(dd_portal, portal_page_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display)
{
    /* Root container for display settings */
    lv_obj_t *disp_container = lv_obj_create(page_display);
    lv_obj_remove_style_all(disp_container);
    lv_obj_set_size(disp_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(disp_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(disp_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(disp_container, 10, 0);
    lv_obj_set_style_pad_gap(disp_container, 14, 0);
    lv_obj_set_scroll_dir(disp_container, LV_DIR_VER);

    /* --- Brightness --- */
    lv_obj_t *lbl_brightness = lv_label_create(disp_container);
    lv_obj_add_style(lbl_brightness, &ui->styles.small, 0);
    lv_label_set_text(lbl_brightness, "Brightness:");

    lv_obj_t *slider_brightness = lv_slider_create(disp_container);
    lv_obj_set_width(slider_brightness, lv_pct(50));
    lv_slider_set_range(slider_brightness, 1, 100);
    lv_slider_set_value(slider_brightness, ui->brightness, LV_ANIM_OFF);
    bsp_display_brightness_set(ui->brightness);
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(slider_brightness, &ui->styles.medium, 0);

    /* --- Screensaver enable --- */
    ui->screensaver.checkbox = lv_checkbox_create(disp_container);
    lv_checkbox_set_text(ui->screensaver.checkbox, "Enable Screensaver");
    if (ui->screensaver.enabled) {
        lv_obj_add_state(ui->screensaver.checkbox, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ui->screensaver.checkbox, cb_screensaver_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->screensaver.checkbox, &ui->styles.medium, 0);

    /* --- Screensaver brightness --- */
    lv_obj_t *lbl_ss_brightness = lv_label_create(disp_container);
    lv_obj_add_style(lbl_ss_brightness, &ui->styles.small, 0);
    lv_label_set_text(lbl_ss_brightness, "Screensaver Brightness:");

    ui->screensaver.slider_brightness = lv_slider_create(disp_container);
    lv_obj_set_width(ui->screensaver.slider_brightness, lv_pct(50));
    lv_slider_set_range(ui->screensaver.slider_brightness, 0, 100);
    lv_slider_set_value(ui->screensaver.slider_brightness, ui->screensaver.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui->screensaver.slider_brightness, slider_ss_brightness_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->screensaver.slider_brightness, &ui->styles.medium, 0);

    /* --- Screensaver timeout section --- */
    lv_obj_t *lbl_ss_time = lv_label_create(disp_container);
    lv_obj_add_style(lbl_ss_time, &ui->styles.small, 0);
    lv_label_set_text(lbl_ss_time, "Screen Timeout (min, 0=off):");

    /* Row container for spinbox + buttons */
    lv_obj_t *timeout_row = lv_obj_create(disp_container);
    lv_obj_remove_style_all(timeout_row);
    lv_obj_set_width(timeout_row, lv_pct(100));
    lv_obj_set_height(timeout_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(timeout_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(timeout_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(timeout_row, 10, 0);
    lv_obj_set_flex_align(timeout_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Decrement button */
    lv_obj_t *btn_dec = lv_btn_create(timeout_row);
    lv_obj_set_size(btn_dec, 40, 40);
    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_dec);
    lv_obj_add_event_cb(btn_dec, spinbox_ss_time_decrement_event_cb, LV_EVENT_ALL, ui);

    /* Timeout spinbox */
    ui->screensaver.spinbox_timeout = lv_spinbox_create(timeout_row);
    lv_spinbox_set_range(ui->screensaver.spinbox_timeout, 0, 30);
    lv_spinbox_set_value(ui->screensaver.spinbox_timeout, ui->screensaver.timeout / 60);
    lv_spinbox_set_digit_format(ui->screensaver.spinbox_timeout, 2, 0);
    lv_obj_set_width(ui->screensaver.spinbox_timeout, 80);
    lv_obj_add_event_cb(ui->screensaver.spinbox_timeout, spinbox_ss_time_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->screensaver.spinbox_timeout, &ui->styles.small, 0);

    /* Increment button */
    lv_obj_t *btn_inc = lv_btn_create(timeout_row);
    lv_obj_set_size(btn_inc, 40, 40);
    lv_obj_t *lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_inc);
    lv_obj_add_event_cb(btn_inc, spinbox_ss_time_increment_event_cb, LV_EVENT_ALL, ui);

    /* --- UI View Selection --- */
    lv_obj_t *lbl_view_mode = lv_label_create(disp_container);
    lv_obj_add_style(lbl_view_mode, &ui->styles.small, 0);
    lv_label_set_text(lbl_view_mode, "Live Display Mode:");

    ui->view_selection.dropdown = lv_dropdown_create(disp_container);
    lv_obj_set_width(ui->view_selection.dropdown, lv_pct(70));
    lv_dropdown_set_options(ui->view_selection.dropdown, 
        "Auto Detection\n"
        "Default Battery View\n"
        "Solar Charger View\n"
        "Battery Monitor View\n"
        "Inverter View\n"
        "DC/DC Converter View"
    );
    
    /* Load saved view mode */
    uint8_t saved_mode = 1; // Default to UI_VIEW_MODE_DEFAULT_BATTERY
    if (load_ui_view_mode(&saved_mode) == ESP_OK) {
        ui->view_selection.mode = (ui_view_mode_t)saved_mode;
    } else {
        ui->view_selection.mode = UI_VIEW_MODE_DEFAULT_BATTERY;
    }
    lv_dropdown_set_selected(ui->view_selection.dropdown, (uint16_t)ui->view_selection.mode);
    
    lv_obj_add_event_cb(ui->view_selection.dropdown, view_selection_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_style(ui->view_selection.dropdown, &ui->styles.small, 0);

    /* --- Timer setup --- */
    ui->screensaver.timer = lv_timer_create(screensaver_timer_cb,
                                            ui->screensaver.timeout * 1000,  /* timeout en segundos */
                                            ui);
    if (ui->screensaver.enabled) {
        lv_timer_reset(ui->screensaver.timer);
        lv_timer_resume(ui->screensaver.timer);
    } else {
        lv_timer_pause(ui->screensaver.timer);
    }
}


static void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron)
{
    /* Root container for Victron keys settings */
    lv_obj_t *victron_container = lv_obj_create(page_victron);
    lv_obj_remove_style_all(victron_container);
    lv_obj_set_size(victron_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(victron_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(victron_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(victron_container, 10, 0);
    lv_obj_set_style_pad_gap(victron_container, 14, 0);
    lv_obj_set_scroll_dir(victron_container, LV_DIR_VER);

    /* Header text */
    lv_obj_t *lbl_header = lv_label_create(victron_container);
    lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_header, "Configure multiple Victron devices with MAC addresses and AES keys:");

    /* --- Victron devices configuration section --- */
    lv_obj_t *victron_section = lv_obj_create(victron_container);
    lv_obj_remove_style_all(victron_section);
    lv_obj_set_width(victron_section, lv_pct(100));
    lv_obj_set_height(victron_section, LV_SIZE_CONTENT);
    lv_obj_set_layout(victron_section, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(victron_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(victron_section, 14, 0);
    ui->victron_config.container = victron_section;

    /* --- Row for Add / Remove buttons --- */
    lv_obj_t *controls_row = lv_obj_create(victron_section);
    lv_obj_remove_style_all(controls_row);
    lv_obj_set_width(controls_row, lv_pct(100));
    lv_obj_set_height(controls_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(controls_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(controls_row, 10, 0);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Add button */
    ui->victron_config.add_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.add_btn, 36, 36);
    lv_obj_t *lbl_add = lv_label_create(ui->victron_config.add_btn);
    lv_label_set_text(lbl_add, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_add);
    lv_obj_add_event_cb(ui->victron_config.add_btn, victron_config_add_btn_event_cb, LV_EVENT_CLICKED, ui);

    /* Remove button */
    ui->victron_config.remove_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.remove_btn, 36, 36);
    lv_obj_t *lbl_remove = lv_label_create(ui->victron_config.remove_btn);
    lv_label_set_text(lbl_remove, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_remove);
    lv_obj_add_event_cb(ui->victron_config.remove_btn, victron_config_remove_btn_event_cb, LV_EVENT_CLICKED, ui);

    /* --- Victron devices list section --- */
    ui->victron_config.list = lv_obj_create(victron_section);
    lv_obj_remove_style_all(ui->victron_config.list);
    lv_obj_set_width(ui->victron_config.list, lv_pct(100));
    lv_obj_set_height(ui->victron_config.list, LV_SIZE_CONTENT);
    lv_obj_set_layout(ui->victron_config.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui->victron_config.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ui->victron_config.list, 10, 0);
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

    /* Main row container */
    lv_obj_t *row = lv_obj_create(ui->victron_config.list);
    if (row == NULL) { ESP_LOGE("UI", "row create failed idx=%d", (int)index); return; }
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
    lv_obj_set_style_radius(row, 4, 0);

    /* Header row with device number and enabled checkbox */
    lv_obj_t *header_row = lv_obj_create(row);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, lv_pct(100));
    lv_obj_set_height(header_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(header_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(header_row, 10, 0);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Device label */
    lv_obj_t *device_label = lv_label_create(header_row);
    lv_obj_set_style_text_font(device_label, &lv_font_montserrat_20, 0);
    lv_label_set_text_fmt(device_label, "Device %d", (int)(index + 1));

    /* Enabled checkbox */
    lv_obj_t *enabled_cb = lv_checkbox_create(header_row);
    lv_checkbox_set_text(enabled_cb, "Enabled");
    lv_obj_set_style_text_font(enabled_cb, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(enabled_cb, victron_enabled_checkbox_event_cb, LV_EVENT_VALUE_CHANGED, ui);

    /* Device name input */
    lv_obj_t *name_label = lv_label_create(row);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(name_label, "Device Name:");

    lv_obj_t *name_ta = lv_textarea_create(row);
    lv_textarea_set_max_length(name_ta, 31);
    lv_obj_set_width(name_ta, lv_pct(80));
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_placeholder_text(name_ta, "e.g. Solar Charger 1");
    lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    /* MAC address input */
    lv_obj_t *mac_label = lv_label_create(row);
    lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(mac_label, "MAC Address:");

    lv_obj_t *mac_ta = lv_textarea_create(row);
    lv_textarea_set_max_length(mac_ta, 17);
    lv_obj_set_width(mac_ta, lv_pct(60));
    lv_textarea_set_one_line(mac_ta, true);
    lv_textarea_set_placeholder_text(mac_ta, "XX:XX:XX:XX:XX:XX");
    lv_obj_set_style_text_font(mac_ta, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    /* AES Key input */
    lv_obj_t *key_label = lv_label_create(row);
    lv_obj_set_style_text_font(key_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(key_label, "AES Key (32 hex characters):");

    lv_obj_t *key_ta = lv_textarea_create(row);
    lv_textarea_set_max_length(key_ta, 32);
    lv_obj_set_width(key_ta, lv_pct(90));
    lv_textarea_set_one_line(key_ta, true);
    lv_textarea_set_placeholder_text(key_ta, "00000000000000000000000000000000");
    lv_obj_set_style_text_font(key_ta, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    /* --- Device Status Section --- */
    lv_obj_t *status_label = lv_label_create(row);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(status_label, "Live Device Status:");

    lv_obj_t *status_container = lv_obj_create(row);
    lv_obj_remove_style_all(status_container);
    lv_obj_set_width(status_container, lv_pct(100));
    lv_obj_set_height(status_container, LV_SIZE_CONTENT);
    lv_obj_set_layout(status_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_container, 6, 0);  // Increased gap for better readability
    lv_obj_set_style_pad_all(status_container, 10, 0);  // Increased padding
    lv_obj_set_style_bg_opa(status_container, LV_OPA_20, 0);  // More visible background
    lv_obj_set_style_radius(status_container, 4, 0);  // Rounded corners
    lv_obj_set_style_border_width(status_container, 1, 0);  // Add border
    lv_obj_set_style_border_opa(status_container, LV_OPA_30, 0);
    lv_obj_set_style_border_color(status_container, lv_color_hex(0x444444), 0);

    /* Device type label */
    lv_obj_t *device_type_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(device_type_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(device_type_lbl, "Device: --");
    lv_obj_set_style_text_color(device_type_lbl, lv_color_hex(0x888888), 0);

    /* Product name label */
    lv_obj_t *product_name_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(product_name_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(product_name_lbl, "Product: --");
    lv_obj_set_style_text_color(product_name_lbl, lv_color_hex(0x888888), 0);

    /* Live metrics status label - enhanced for detailed status */
    lv_obj_t *error_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(error_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(error_lbl, "Status: Waiting for data...");
    lv_obj_set_style_text_color(error_lbl, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(error_lbl, LV_LABEL_LONG_WRAP);  // Enable text wrapping for longer status
    lv_obj_set_width(error_lbl, lv_pct(100));  // Full width for better text layout

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

    lv_obj_t *main_header = lv_menu_get_main_header(menu);
    lv_obj_set_style_text_font(main_header, &lv_font_montserrat_28, 0);
    lv_obj_set_flex_align(main_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back_btn = lv_menu_get_main_header_back_btn(menu);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_pad_hor(back_btn, 14, 0);
    lv_obj_set_style_pad_ver(back_btn, 8, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3D5A80), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0x00BFFF), 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24, 0);
    /* Spacer invisible para centrar el titulo via SPACE_BETWEEN */
    lv_obj_t *header_spacer = lv_obj_create(main_header);
    lv_obj_remove_style_all(header_spacer);
    lv_obj_set_size(header_spacer, 110, 1);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
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

static void ap_checkbox_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *checkbox = lv_event_get_target(e);
    if (ui == NULL || checkbox == NULL) {
        return;
    }
    bool en = lv_obj_has_state(checkbox, LV_STATE_CHECKED);
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_u8(h, "enabled", en);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG_SETTINGS, "AP %s", en ? "enabled" : "disabled");
    } else {
        ESP_LOGE(TAG_SETTINGS, "nvs_open failed: %s", esp_err_to_name(err));
    }

    if (en) {
        wifi_ap_init();
    } else {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err == ESP_OK) {
            ESP_LOGI(TAG_SETTINGS, "Soft-AP stopped");
        } else {
            ESP_LOGE(TAG_SETTINGS, "Failed to stop AP: %s", esp_err_to_name(stop_err));
        }
    }
}





static void reboot_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG_SETTINGS, "Reboot requested via UI");
    esp_restart();
}

static void brightness_slider_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }
    int val = lv_slider_get_value(lv_event_get_target(e));
    ui->brightness = (uint8_t)val;
    bsp_display_brightness_set(val);
    save_brightness(ui->brightness);
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

static void victron_debug_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->victron_debug_checkbox == NULL) return;
    bool enabled = lv_obj_has_state(ui->victron_debug_checkbox, LV_STATE_CHECKED);
    ui->victron_debug_enabled = enabled;
    if (save_victron_debug(enabled) == ESP_OK) {
        ESP_LOGI(TAG_SETTINGS, "Victron BLE debug %s", enabled ? "enabled" : "disabled");
    } else {
        ESP_LOGE(TAG_SETTINGS, "Failed to persist Victron BLE debug setting");
    }
    // Apply immediately to BLE module
    victron_ble_set_debug(enabled);
}

static void slider_ss_brightness_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.slider_brightness == NULL) {
        return;
    }
    ui->screensaver.brightness = lv_slider_get_value(ui->screensaver.slider_brightness);
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    if (ui->screensaver.active) {
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
    }
}

static void spinbox_ss_time_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.spinbox_timeout == NULL) {
        return;
    }
    ui->screensaver.timeout = lv_spinbox_get_value(ui->screensaver.spinbox_timeout) * 60;
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    if (ui->screensaver.timer) {
        uint32_t ms = ui->screensaver.timeout > 0 ? ui->screensaver.timeout * 1000U : 0xFFFFFFFF;
        lv_timer_set_period(ui->screensaver.timer, ms);
    }
}

static void spinbox_ss_time_increment_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.spinbox_timeout == NULL) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_increment(ui->screensaver.spinbox_timeout);
        ui->screensaver.timeout = lv_spinbox_get_value(ui->screensaver.spinbox_timeout) * 60;
        save_screensaver_settings(ui->screensaver.enabled,
                                  ui->screensaver.brightness,
                                  ui->screensaver.timeout);
        if (ui->screensaver.timer) {
            uint32_t ms = ui->screensaver.timeout > 0 ? ui->screensaver.timeout * 1000U : 0xFFFFFFFF;
            lv_timer_set_period(ui->screensaver.timer, ms);
        }
    }
}

static void spinbox_ss_time_decrement_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->screensaver.spinbox_timeout == NULL) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_decrement(ui->screensaver.spinbox_timeout);
        ui->screensaver.timeout = lv_spinbox_get_value(ui->screensaver.spinbox_timeout) * 60;
        save_screensaver_settings(ui->screensaver.enabled,
                                  ui->screensaver.brightness,
                                  ui->screensaver.timeout);
        if (ui->screensaver.timer) {
            uint32_t ms = ui->screensaver.timeout > 0 ? ui->screensaver.timeout * 1000U : 0xFFFFFFFF;
            lv_timer_set_period(ui->screensaver.timer, ms);
        }
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

static void screensaver_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : NULL;
    if (ui == NULL) {
        return;
    }
    if (ui->screensaver.enabled && !ui->screensaver.active) {
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
            bsp_display_brightness_set(ui->brightness);
            ui->screensaver.active = false;
        }
    }
}



static void victron_config_add_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    if (ui->victron_config.count >= UI_MAX_VICTRON_DEVICES) {
        return;
    }

    size_t index = ui->victron_config.count;
    ui->victron_config.count++;

    victron_config_create_row(ui, index);
    victron_config_update_controls(ui);
    victron_config_persist(ui);
}

static void victron_config_remove_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    if (ui->victron_config.count == 0) {
        return;
    }

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

static void victron_enabled_checkbox_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    victron_config_persist(ui);
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

static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page)
{
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
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_REFRESH "  Estado");

    ui->lbl_about_uptime = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_uptime, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(ui->lbl_about_heap, &lv_font_montserrat_20, 0);
    lv_label_set_text(ui->lbl_about_heap, "RAM libre: --");

    ui->lbl_about_sd = lv_label_create(row_mem);
    lv_obj_set_style_text_font(ui->lbl_about_sd, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(ui->lbl_about_sd, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(ui->lbl_about_sd, "SD: --");

    ui->lbl_about_ip = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_ip, &lv_font_montserrat_20, 0);
    lv_label_set_text(ui->lbl_about_ip, "IP AP: --");

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
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_LIST "  Version, Repo y Creditos");

    /* Boton Reiniciar pequeno en la esquina */
    lv_obj_t *btn_reboot_hdr = lv_btn_create(card3_header);
    lv_obj_set_size(btn_reboot_hdr, 130, 40);
    lv_obj_set_style_bg_color(btn_reboot_hdr, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_reboot_hdr, 8, 0);
    lv_obj_t *lbl_reboot_hdr = lv_label_create(btn_reboot_hdr);
    lv_label_set_text(lbl_reboot_hdr, LV_SYMBOL_POWER "  Reiniciar");
    lv_obj_set_style_text_font(lbl_reboot_hdr, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_reboot_hdr);
    lv_obj_add_event_cb(btn_reboot_hdr, reboot_btn_cb, LV_EVENT_CLICKED, ui);

    /* Version */
    lv_obj_t *lbl_ver_top = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_ver_top, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ver_top, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text_fmt(lbl_ver_top, "Version: %s", APP_VERSION);

    /* Chip + IDF */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    lv_obj_t *lbl_chip = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_chip, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_chip, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text_fmt(lbl_chip, "ESP32 model=%d cores=%d rev=%d  |  IDF: %s",
        chip.model, chip.cores, chip.revision, esp_get_idf_version());

    lv_obj_t *lbl_port = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_port, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_port, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(lbl_port, "Port para Guition JC1060P470C_I por Ehuntabi");

    lv_obj_t *lbl_gh = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_gh, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_gh, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(lbl_gh, "github.com/Ehuntabi/victron-jc1060p470c-esp32p4");

    lv_obj_t *lbl_cred = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_cred, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cred, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_cred, "Basado en: CamdenSutherland, wytr");

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
    ESP_LOGI(TAG_SETTINGS, "Portal page: %s", sel == 0 ? "Keys" : "Logs");
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
    /* Forzar ancho ~48% para 2 columnas */
    lv_obj_set_width(cont, lv_pct(48));
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = lv_label_create(cont);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &ui->styles.medium, 0);
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
    about_refresh_dynamic((ui_state_t *)t->user_data);
}

static void reboot_msgbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    if (btn_id == 0) {
        ESP_LOGW(TAG_SETTINGS, "Reboot confirmed by user");
        lv_msgbox_close(mbox);
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
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    lv_obj_t *btn_frigo = lv_btn_create(cont);
    lv_obj_set_size(btn_frigo, lv_pct(100), 80);
    lv_obj_set_style_bg_color(btn_frigo, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_radius(btn_frigo, 10, 0);
    lv_obj_t *lbl_frigo = lv_label_create(btn_frigo);
    lv_label_set_text(lbl_frigo, "FRIGO");
    lv_obj_set_style_text_font(lbl_frigo, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_frigo, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_frigo);
    lv_obj_add_event_cb(btn_frigo, logs_btn_frigo_cb, LV_EVENT_CLICKED, ui);

    lv_obj_t *btn_bat = lv_btn_create(cont);
    lv_obj_set_size(btn_bat, lv_pct(100), 80);
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
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_VOLUME_MAX "  Sonido");

    /* Volumen */
    lv_obj_t *lbl_vol = lv_label_create(card1);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_mute, "Silenciar avisos");

    lv_obj_t *sw = lv_switch_create(row_mute);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4FC3F7), LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (audio_is_muted()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sound_mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24, 0);
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
    lv_obj_set_style_text_font(lbl_crit, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(lbl_warn, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24, 0);
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
    lv_obj_set_style_text_font(lbl_min_a, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(lbl_t_a, &lv_font_montserrat_20, 0);
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
