/* ui.c */
#include "ui.h"
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <lvgl.h>
#include "esp_lvgl_port.h"  // lv_port.h sustituido por esp_lvgl_port
#include "esp_log.h"
#include "victron_ble.h"
#include "battery_history.h"
#include "victron_products.h"
#include "nvs_flash.h"
#include "config_storage.h"
#include <stdio.h>
#include "ui/ui_state.h"
#include "ui/device_view.h"
#include "ui/view_registry.h"
#include "ui/settings_panel.h"
#include "ui/view_default_battery.h"
#include "rtc_rx8025t.h"
#include "datalogger.h"
#include <time.h>
#include <sys/time.h>

#include "esp_timer.h"

static int64_t s_last_ble_data_us = 0;
static void ble_indicator_timer_cb(lv_timer_t *t);


// Font Awesome symbols (declared in main.c)
LV_FONT_DECLARE(font_awesome_solar_panel_40);
LV_FONT_DECLARE(font_awesome_bolt_40);

static const char *TAG_UI = "UI_MODULE";

static ui_state_t g_ui = {
    .brightness = 100,
    .current_device_type = VICTRON_BLE_RECORD_TEST,
    .current_product_id = 0,
    .has_received_data = false,
    .tab_settings_index = UINT16_MAX,
    .default_view = NULL,
    .victron_config = {0},
    .current_device_mac = "00:00:00:00:00:00",
    .view_selection = { .mode = UI_VIEW_MODE_DEFAULT_BATTERY, .dropdown = NULL },
    .lbl_device_type = NULL,
    .lbl_product_name = NULL,
    .lbl_error = NULL,
    .ta_mac = NULL,
    .ta_key = NULL,
};

// Forward declarations
static void tabview_touch_event_cb(lv_event_t *e);
static void frigo_swipe_cb(lv_event_t *e);
static void clock_click_cb(lv_event_t *e);
static void ensure_device_layout(ui_state_t *ui, victron_record_type_t type);
static const char *device_type_name(victron_record_type_t type);
static void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size);
static void ui_update_device_activity(ui_state_t *ui, const char *mac_address);
static void ui_check_device_timeouts(lv_timer_t *timer);
static void clock_timer_cb(lv_timer_t *timer);

static bool obj_is_descendant(const lv_obj_t *obj, const lv_obj_t *parent)
{
    if (obj == NULL || parent == NULL) {
        return false;
    }
    const lv_obj_t *current = obj;
    while (current != NULL) {
        if (current == parent) {
            return true;
        }
        current = lv_obj_get_parent(current);
    }
    return false;
}

void ui_init(void) {
    ui_state_t *ui = &g_ui;

    load_brightness(&ui->brightness);

    ui->active_view = NULL;
    ui->default_view = NULL;
    ui->current_device_type = VICTRON_BLE_RECORD_TEST;
    strcpy(ui->current_device_mac, "00:00:00:00:00:00");
    ui->ta_mac = NULL;          // Legacy field - no longer created in System page
    ui->ta_key = NULL;          // Legacy field - no longer created in System page  
    ui->lbl_device_type = NULL; // Legacy field - no longer created in System page
    ui->lbl_product_name = NULL;// Legacy field - no longer created in System page
    ui->lbl_error = NULL;       // Legacy field - no longer created in System page
    for (size_t i = 0; i < UI_MAX_DEVICE_VIEWS; ++i) {
        ui->views[i] = NULL;
    }


    ui->victron_config.count = 0;
    ui->victron_config.container = NULL;
    ui->victron_config.list = NULL;
    ui->victron_config.add_btn = NULL;
    ui->victron_config.remove_btn = NULL;
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
        
        // Initialize device activity tracking
        ui->last_active_devices[i][0] = '\0';
        ui->last_activity_time[i] = 0;
    }
    
    // Create timer to check for device timeouts (check every 10 seconds)
    ui->device_timeout_timer = lv_timer_create(ui_check_device_timeouts, 10000, ui);

    /* Initialize view selection - load saved mode or use default */
    uint8_t saved_mode = 1; // Default to UI_VIEW_MODE_DEFAULT_BATTERY
    if (load_ui_view_mode(&saved_mode) == ESP_OK) {
        ui->view_selection.mode = (ui_view_mode_t)saved_mode;
    } else {
        ui->view_selection.mode = UI_VIEW_MODE_DEFAULT_BATTERY;
    }
    ui->view_selection.dropdown = NULL;


    char default_ssid[33]; size_t ssid_len = sizeof(default_ssid);
    char default_pass[65]; size_t pass_len = sizeof(default_pass);
    uint8_t ap_enabled;
    esp_err_t wifi_err = load_wifi_config(default_ssid, &ssid_len, default_pass, &pass_len, &ap_enabled);
    if (wifi_err != ESP_OK) {
        strncpy(default_ssid, "VictronConfig", sizeof(default_ssid));
        default_ssid[sizeof(default_ssid) - 1] = '\0';
        strncpy(default_pass, DEFAULT_AP_PASSWORD, sizeof(default_pass));
        default_pass[sizeof(default_pass) - 1] = '\0';
        ap_enabled = 1;
        save_wifi_config(default_ssid, default_pass, ap_enabled);
    } else if (default_pass[0] == '\0') {
        strncpy(default_pass, DEFAULT_AP_PASSWORD, sizeof(default_pass));
        default_pass[sizeof(default_pass) - 1] = '\0';
        save_wifi_config(default_ssid, default_pass, ap_enabled);
    }

    load_screensaver_settings(&ui->screensaver.enabled,
                              &ui->screensaver.brightness,
                              &ui->screensaver.timeout);

#if LV_USE_THEME_DEFAULT
    lv_theme_default_init(NULL,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        LV_THEME_DEFAULT_DARK,
        &lv_font_montserrat_28
    );
#endif

    ui->tabview   = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 60);
    lv_obj_add_flag(ui->tabview, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(ui->tabview, LV_OBJ_FLAG_SCROLLABLE);
    /* Fuente grande para los tabs */
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(ui->tabview);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_28, 0);
    ui->tab_live  = lv_tabview_add_tab(ui->tabview, "Live");
    ui->tab_settings = lv_tabview_add_tab(ui->tabview, "Settings");

    ui->tab_settings_index = lv_obj_get_index(ui->tab_settings);

    /* Reloj en barra superior — esquina derecha */
    ui->lbl_clock = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(ui->lbl_clock, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ui->lbl_clock, lv_color_white(), 0);
    lv_label_set_text(ui->lbl_clock, "00:00");
    lv_obj_align(ui->lbl_clock, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_obj_set_style_bg_opa(ui->lbl_clock, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_clock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_clock, 4, 0);
    lv_obj_set_style_radius(ui->lbl_clock, 4, 0);
    /* Indicador BLE - centro inferior */
    ui->lbl_ble = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(ui->lbl_ble, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->lbl_ble, "BLE: --");
    lv_obj_align(ui->lbl_ble, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(ui->lbl_ble, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_ble, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_ble, 4, 0);
    lv_obj_set_style_radius(ui->lbl_ble, 4, 0);

    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);


    ui->keyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(ui->keyboard, LV_HOR_RES, LV_VER_RES/2);
    lv_obj_align(ui->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);

    // Styles
    lv_style_init(&ui->styles.small);
    /* Use montserrat 22 for titles as requested */
lv_style_set_text_font(&ui->styles.small, &lv_font_montserrat_28);
    lv_style_set_text_color(&ui->styles.small, lv_color_white());

    lv_style_init(&ui->styles.medium);
    lv_style_set_text_font(&ui->styles.medium, &lv_font_montserrat_36);
    lv_style_set_text_color(&ui->styles.medium, lv_color_white());

    lv_style_init(&ui->styles.big);
    lv_style_set_text_font(&ui->styles.big, &lv_font_montserrat_46);
    lv_style_set_text_color(&ui->styles.big, lv_color_white());

    lv_style_init(&ui->styles.value);
lv_style_set_text_font(&ui->styles.value, &lv_font_montserrat_32);
    lv_style_set_text_color(&ui->styles.value, lv_color_white());

    // Create default battery view instead of "No live data" label
    ui->default_view = ui_default_battery_view_create(ui, ui->tab_live);
    
    // Show the default view initially (will be updated when data arrives based on view selection)
    if (ui->default_view && ui->default_view->show) {
        ui->default_view->show(ui->default_view);
    }
    
    // Keep the old label for compatibility but hide it
    ui->lbl_no_data = lv_label_create(ui->tab_live);
    lv_label_set_text(ui->lbl_no_data, "No live data received yet");
    lv_obj_add_style(ui->lbl_no_data, &ui->styles.medium, 0);
    lv_label_set_long_mode(ui->lbl_no_data, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->lbl_no_data, lv_pct(90));
    lv_obj_set_style_text_align(ui->lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui->lbl_no_data);
    lv_obj_add_flag(ui->lbl_no_data, LV_OBJ_FLAG_HIDDEN); // Hide by default

    ui_settings_panel_init(ui, default_ssid, default_pass, ap_enabled);

    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);
    lv_timer_create(clock_timer_cb, 30000, ui);
    lv_timer_create(ble_indicator_timer_cb, 1000, ui);
    clock_timer_cb(NULL);
    lvgl_port_unlock();
}

void ui_on_panel_data(const victron_data_t *d) {
    if (d == NULL) {
        return;
    }

    ui_state_t *ui = &g_ui;

    lvgl_port_lock(0);

    if (ui->lbl_ble) {
        lv_label_set_text(ui->lbl_ble, "BLE: OK");
        lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x00C851), 0);
    }
    s_last_ble_data_us = esp_timer_get_time();

    /* Battery history: alimenta el modulo con la corriente del dispositivo */
    switch (d->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR:
            battery_history_update_latest(BH_SRC_BATTERY_MONITOR,
                d->record.battery.battery_current_milli);
            break;
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:
            battery_history_update_latest(BH_SRC_SOLAR_CHARGER,
                (int32_t)d->record.solar.battery_current_deci * 100);
            break;
        case VICTRON_BLE_RECORD_ORION_XS:
            /* Output current = into battery side */
            battery_history_update_latest(BH_SRC_ORION_XS,
                (int32_t)d->record.orion.output_current_deci * 100);
            break;
        case VICTRON_BLE_RECORD_AC_CHARGER:
            battery_history_update_latest(BH_SRC_AC_CHARGER,
                (int32_t)d->record.ac_charger.battery_current_1_deci * 100);
            break;
        default:
            break;
    }

    if (!ui->has_received_data) {
        ui->has_received_data = true;
        if (ui->lbl_no_data) {
            lv_obj_add_flag(ui->lbl_no_data, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Always update the default view with incoming data (it handles multiple device types)
    if (ui->default_view && ui->default_view->update) {
        ui->default_view->update(ui->default_view, d);
    }

    const char *type_str = device_type_name(d->type);
    
    // Update legacy labels (if they exist for compatibility)
    if (ui->lbl_device_type) {
        lv_label_set_text_fmt(ui->lbl_device_type, "Device: %s", type_str);
    }

    ui->current_product_id = d->product_id;
    if (ui->lbl_product_name) {
        if (d->product_id != 0) {
            const char *prod_name = victron_product_name(d->product_id);
            if (prod_name != NULL) {
                lv_label_set_text_fmt(ui->lbl_product_name,
                                      "Product: %s (0x%04X)",
                                      prod_name,
                                      (unsigned)d->product_id);
            } else {
                lv_label_set_text_fmt(ui->lbl_product_name,
                                      "Product: 0x%04X",
                                      (unsigned)d->product_id);
            }
        } else {
            lv_label_set_text(ui->lbl_product_name, "Product: --");
        }
    }
    
    // Prepare product info for device status updates
    char product_info[128] = {0};
    if (d->product_id != 0) {
        const char *prod_name = victron_product_name(d->product_id);
        if (prod_name != NULL) {
            snprintf(product_info, sizeof(product_info), "%s (0x%04X)", prod_name, (unsigned)d->product_id);
        } else {
            snprintf(product_info, sizeof(product_info), "0x%04X", (unsigned)d->product_id);
        }
    } else {
        strcpy(product_info, "--");
    }

    ensure_device_layout(ui, d->type);

    if (ui->active_view && ui->active_view->update) {
        ui->active_view->update(ui->active_view, d);
        
        // Prepare detailed status information based on device type
        char detailed_status[256] = {0};
        ui_prepare_detailed_device_status(d, detailed_status, sizeof(detailed_status));
        
        // Update device activity tracking
        ui_update_device_activity(ui, ui->current_device_mac);
        
        // Update successful data reception status in Victron Keys page
        ui_settings_panel_update_victron_device_status(ui, ui->current_device_mac, type_str, product_info, detailed_status);
    } else {
        // Update error status in Victron Keys page
        const char *error_msg = "No renderer for device type";
        if (d->type == VICTRON_BLE_RECORD_TEST) {
            error_msg = "Unknown device type";
        }
        
        // Update legacy error label (if it exists)
        if (ui->lbl_error) {
            lv_label_set_text(ui->lbl_error, error_msg);
        }
        
        // Update error status for this device in Victron Keys page
        ui_settings_panel_update_victron_device_status(ui, ui->current_device_mac, type_str, product_info, error_msg);
    }

    lvgl_port_unlock();
}

void ui_force_view_update(void)
{
    ui_state_t *ui = &g_ui;
    lvgl_port_lock(0);
    
    // Force a layout update regardless of current device type
    victron_record_type_t saved_type = ui->current_device_type;
    ui->current_device_type = VICTRON_BLE_RECORD_TEST; // Reset to force update
    ensure_device_layout(ui, saved_type);
    
    lvgl_port_unlock();
}

static void clock_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : ui_get_state();
    if (!ui || !ui->lbl_clock) return;

    /* Usar el reloj del sistema en lugar de leer el RTC cada vez */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);

    if (t.tm_year > 100) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%02d:%02d  %02d/%02d/%04d",
                 t.tm_hour, t.tm_min,
                 t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        lv_label_set_text(ui->lbl_clock, buf);
    }
}

void ui_notify_user_activity(void)
{
    ui_state_t *ui = &g_ui;
    ui_settings_panel_on_user_activity(ui);
}

void ui_set_ble_mac(const uint8_t *mac) {
    // Format MAC as "XX:XX:XX:XX:XX:XX"
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
    ui_state_t *ui = &g_ui;
    lvgl_port_lock(0);
    
    // Store current MAC address
    strcpy(ui->current_device_mac, mac_str);
    
    // Legacy MAC field update (if it exists)
    ui_settings_panel_set_mac(ui, mac_str);
    lvgl_port_unlock();
}

static void ensure_device_layout(ui_state_t *ui, victron_record_type_t type)
{
    if (ui == NULL) {
        return;
    }

    // Check if we should use manual view selection instead of auto detection
    if (ui->view_selection.mode != UI_VIEW_MODE_AUTO) {
        // Manual view mode selected - determine which view to show
        victron_record_type_t target_type = VICTRON_BLE_RECORD_TEST;
        bool show_default = true;
        
        switch (ui->view_selection.mode) {
            case UI_VIEW_MODE_DEFAULT_BATTERY:
                show_default = true;
                break;
            case UI_VIEW_MODE_SOLAR_CHARGER:
                target_type = VICTRON_BLE_RECORD_SOLAR_CHARGER;
                show_default = false;
                break;
            case UI_VIEW_MODE_BATTERY_MONITOR:
                target_type = VICTRON_BLE_RECORD_BATTERY_MONITOR;
                show_default = false;
                break;
            case UI_VIEW_MODE_INVERTER:
                target_type = VICTRON_BLE_RECORD_INVERTER;
                show_default = false;
                break;
            case UI_VIEW_MODE_DCDC_CONVERTER:
                target_type = VICTRON_BLE_RECORD_DCDC_CONVERTER;
                show_default = false;
                break;
            default:
                show_default = true;
                break;
        }
        
        if (show_default) {
            // Show default battery view
            if (ui->active_view && ui->active_view->hide) {
                ui->active_view->hide(ui->active_view);
            }
            ui->active_view = NULL;
            if (ui->default_view && ui->default_view->show) {
                ui->default_view->show(ui->default_view);
            }
        } else {
            // Show specific view type regardless of received data type
            if (ui->active_view && ui->active_view->hide) {
                ui->active_view->hide(ui->active_view);
            }
            
            ui->active_view = NULL;
            ui_device_view_t *view = ui_view_registry_ensure(ui, target_type, ui->tab_live);
            if (view && view->show) {
                if (ui->default_view && ui->default_view->hide) {
                    ui->default_view->hide(ui->default_view);
                }
                view->show(view);
                ui->active_view = view;
            } else {
                // Fallback to default view if specific view not available
                if (ui->default_view && ui->default_view->show) {
                    ui->default_view->show(ui->default_view);
                }
                ESP_LOGW(TAG_UI, "Requested view type 0x%02X not available, showing default", (unsigned)target_type);
            }
        }
        
        ui->current_device_type = show_default ? VICTRON_BLE_RECORD_TEST : target_type;
        return;
    }

    // Auto detection mode - original logic
    if (type == ui->current_device_type) {
        return;
    }

    if (ui->active_view && ui->active_view->hide) {
        ui->active_view->hide(ui->active_view);
    }

    ui->active_view = NULL;

    ui_device_view_t *view = ui_view_registry_ensure(ui, type, ui->tab_live);
    if (view && view->show) {
        // Hide default view when showing specific device view
        if (ui->default_view && ui->default_view->hide) {
            ui->default_view->hide(ui->default_view);
        }
        view->show(view);
        ui->active_view = view;
    } else {
        // No specific view available, show default view
        if (ui->default_view && ui->default_view->show) {
            ui->default_view->show(ui->default_view);
        }
        if (type != VICTRON_BLE_RECORD_TEST) {
            ESP_LOGW(TAG_UI, "No view available for device type 0x%02X, showing default", (unsigned)type);
        }
    }

    ui->current_device_type = type;
}

static const char *device_type_name(victron_record_type_t type)
{
    return ui_view_registry_name(type);
}

static void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size)
{
    if (data == NULL || status_out == NULL || status_size == 0) {
        return;
    }

    switch (data->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            const victron_record_battery_monitor_t *batt = &data->record.battery;
            if (batt->soc_deci_percent != 0xFFFF && batt->battery_voltage_centi > 0) {
                uint16_t soc_pct = batt->soc_deci_percent / 10;
                uint16_t soc_dec = batt->soc_deci_percent % 10;
                uint16_t volts = batt->battery_voltage_centi / 100;
                uint16_t hundredths = batt->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u.%u%% | Voltage: %u.%02uV", 
                         soc_pct, soc_dec, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Battery Monitor");
            }
            break;
        }

        case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
            const victron_record_solar_charger_t *solar = &data->record.solar;
            if (solar->pv_power_w > 0 && solar->battery_voltage_centi > 0) {
                uint16_t volts = solar->battery_voltage_centi / 100;
                uint16_t hundredths = solar->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "Power: %uW | Battery: %u.%02uV", 
                         solar->pv_power_w, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Solar Charger");
            }
            break;
        }

        case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
            const victron_record_lynx_smart_bms_t *bms = &data->record.lynx;
            if (bms->soc_deci_percent > 0 && bms->battery_voltage_centi > 0) {
                uint16_t soc_pct = bms->soc_deci_percent / 10;
                uint16_t soc_dec = bms->soc_deci_percent % 10;
                uint16_t volts = bms->battery_voltage_centi / 100;
                uint16_t hundredths = bms->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u.%u%% | Voltage: %u.%02uV", 
                         soc_pct, soc_dec, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Lynx Smart BMS");
            }
            break;
        }

        case VICTRON_BLE_RECORD_INVERTER: {
            const victron_record_inverter_t *inv = &data->record.inverter;
            if (inv->ac_apparent_power_va > 0 && inv->battery_voltage_centi > 0) {
                uint16_t volts = inv->battery_voltage_centi / 100;
                uint16_t hundredths = inv->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "Power: %uVA | Battery: %u.%02uV", 
                         inv->ac_apparent_power_va, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Inverter");
            }
            break;
        }

        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            const victron_record_dcdc_converter_t *dcdc = &data->record.dcdc;
            if (dcdc->input_voltage_centi > 0 && dcdc->output_voltage_centi > 0) {
                uint16_t in_volts = dcdc->input_voltage_centi / 100;
                uint16_t in_hundredths = dcdc->input_voltage_centi % 100;
                uint16_t out_volts = dcdc->output_voltage_centi / 100;
                uint16_t out_hundredths = dcdc->output_voltage_centi % 100;
                snprintf(status_out, status_size, "In: %u.%02uV | Out: %u.%02uV", 
                         in_volts, in_hundredths, out_volts, out_hundredths);
            } else {
                snprintf(status_out, status_size, "Active - DC/DC Converter");
            }
            break;
        }

        case VICTRON_BLE_RECORD_ORION_XS: {
            const victron_record_orion_xs_t *orion = &data->record.orion;
            if (orion->input_voltage_centi > 0 && orion->output_voltage_centi > 0) {
                uint16_t in_volts = orion->input_voltage_centi / 100;
                uint16_t in_hundredths = orion->input_voltage_centi % 100;
                uint16_t out_volts = orion->output_voltage_centi / 100;
                uint16_t out_hundredths = orion->output_voltage_centi % 100;
                snprintf(status_out, status_size, "In: %u.%02uV | Out: %u.%02uV", 
                         in_volts, in_hundredths, out_volts, out_hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Orion XS");
            }
            break;
        }

        case VICTRON_BLE_RECORD_VE_BUS: {
            const victron_record_ve_bus_t *vebus = &data->record.vebus;
            if (vebus->soc_percent > 0 && vebus->battery_voltage_centi > 0) {
                uint16_t volts = vebus->battery_voltage_centi / 100;
                uint16_t hundredths = vebus->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u%% | Battery: %u.%02uV", 
                         vebus->soc_percent, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - VE.Bus System");
            }
            break;
        }

        default:
            snprintf(status_out, status_size, "Active - Device Connected");
            break;
    }
}

static void ui_update_device_activity(ui_state_t *ui, const char *mac_address)
{
    if (ui == NULL || mac_address == NULL) {
        return;
    }
    
    // Get current time in milliseconds
    uint32_t current_time = lv_tick_get();
    
    // Find existing entry or empty slot
    int slot = -1;
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (strcmp(ui->last_active_devices[i], mac_address) == 0) {
            // Found existing entry
            slot = i;
            break;
        }
        if (slot == -1 && ui->last_active_devices[i][0] == '\0') {
            // Found empty slot
            slot = i;
        }
    }
    
    if (slot >= 0) {
        // Update activity record
        strncpy(ui->last_active_devices[slot], mac_address, sizeof(ui->last_active_devices[slot]) - 1);
        ui->last_active_devices[slot][sizeof(ui->last_active_devices[slot]) - 1] = '\0';
        ui->last_activity_time[slot] = current_time;
    }
}

static void ui_check_device_timeouts(lv_timer_t *timer)
{
    ui_state_t *ui = (ui_state_t *)timer->user_data;
    if (ui == NULL) {
        return;
    }
    
    uint32_t current_time = lv_tick_get();
    const uint32_t timeout_ms = 30000; // 30 seconds timeout
    
    // Check each tracked device for timeout
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (ui->last_active_devices[i][0] != '\0') {
            uint32_t time_since_last = current_time - ui->last_activity_time[i];
            
            if (time_since_last > timeout_ms) {
                // Device has timed out - mark as offline
                ui_settings_panel_update_victron_device_status(ui, ui->last_active_devices[i], 
                                                              "", "", "Offline - No data received");
                
                // Clear the tracking entry
                ui->last_active_devices[i][0] = '\0';
                ui->last_activity_time[i] = 0;
            }
        }
    }
}

static void tabview_touch_event_cb(lv_event_t *e) {
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED || ui->keyboard == NULL) {
        return;
    }

    if (lv_obj_has_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    if (obj_is_descendant(target, ui->keyboard)) {
        return;
    }

    if (ui->wifi.password_toggle != NULL &&
        obj_is_descendant(target, ui->wifi.password_toggle)) {
        return;
    }

    lv_obj_t *ta = lv_keyboard_get_textarea(ui->keyboard);
    if (obj_is_descendant(target, ta)) {
        return;
    }

    if (ta != NULL) {
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_event_send(ta, LV_EVENT_DEFOCUSED, NULL);
    } else {
        lv_keyboard_set_textarea(ui->keyboard, NULL);
        lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_obj_set_height(ui->tabview, screen_h);
        lv_obj_update_layout(ui->tabview);
    }
}

void ui_mark_device_offline(const char *mac_address)
{
    if (mac_address == NULL) {
        return;
    }
    
    ui_state_t *ui = &g_ui;
    
    // Update device status to offline
    ui_settings_panel_update_victron_device_status(ui, mac_address, "", "", "Offline - Connection lost");
    
    // Remove from activity tracking
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (strcmp(ui->last_active_devices[i], mac_address) == 0) {
            ui->last_active_devices[i][0] = '\0';
            ui->last_activity_time[i] = 0;
            break;
        }
    }
}

void ui_refresh_victron_device_list(void)
{
    ui_state_t *ui = &g_ui;
    ESP_LOGI("ui", "Refreshing Victron device list in settings panel");
    if (lvgl_port_lock(0)) {
        ui_settings_panel_refresh_victron_devices(ui);
        lvgl_port_unlock();
    }
}
// force font update
// force tab font

ui_state_t *ui_get_state(void) { return &g_ui; }

void ui_set_freezer_alarm(ui_state_t *ui, bool active)
{
    if (!ui) return;

    /* Crear borde la primera vez */
    if (!ui->alarm_border) {
        ui->alarm_border = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(ui->alarm_border);
        lv_obj_set_size(ui->alarm_border, LV_HOR_RES, LV_VER_RES);
        lv_obj_align(ui->alarm_border, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_width(ui->alarm_border, 8, 0);
        lv_obj_set_style_border_color(ui->alarm_border, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_opa(ui->alarm_border, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ui->alarm_border, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ui->alarm_border, LV_OBJ_FLAG_HIDDEN);
    }

    if (active) {
        lv_obj_clear_flag(ui->alarm_border, LV_OBJ_FLAG_HIDDEN);
        /* Parpadeo con animacion de opacidad */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui->alarm_border);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_border_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 600);
        lv_anim_set_playback_time(&a, 600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        ESP_LOGW("UI", "ALARMA CONGELADOR activa");
    } else {
        lv_anim_del(ui->alarm_border, NULL);
        lv_obj_add_flag(ui->alarm_border, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Pantalla gráfica temperaturas ─────────────────────────── */
static lv_obj_t *s_chart      = NULL;
static lv_chart_series_t *s_ser_aletas     = NULL;
static lv_chart_series_t *s_ser_congelador = NULL;
static lv_chart_series_t *s_ser_exterior   = NULL;
static lv_chart_series_t *s_ser_fan        = NULL;

static void chart_screen_close_cb(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    lv_obj_del(screen);
    s_chart = NULL;
}

void ui_show_chart_screen(ui_state_t *ui)
{
    if (!ui) return;

    /* Crear pantalla a pantalla completa */
    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_move_foreground(scr);

    /* Título */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_label_set_text(lbl_title, "Temperaturas");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 8);

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, chart_screen_close_cb, LV_EVENT_CLICKED, scr);

    /* Leyenda */
    const char *leyenda[] = {"Aletas", "Congel.", "Exter.", "Fan%"};
    lv_color_t colores[]  = {
        lv_color_hex(0x00BFFF),
        lv_color_hex(0xFF4444),
        lv_color_hex(0x44FF44),
        lv_color_hex(0xFFAA00)
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_bg_color(dot, colores[i], 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_set_pos(dot, 10 + i * 120, 55);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, colores[i], 0);
        lv_label_set_text(lbl, leyenda[i]);
        lv_obj_set_pos(lbl, 30 + i * 120, 50);
    }

    /* Gráfica */
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, LV_HOR_RES - 20, LV_VER_RES - 130);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(s_chart, 5, 10);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x333333), LV_PART_MAIN);

    int count = datalogger_get_count();
    lv_chart_set_point_count(s_chart, count > 0 ? count : 2);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -20, 15);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y,   8, 4, 5, 1, true, 50);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_SECONDARY_Y, 8, 4, 5, 1, true, 50);
    lv_obj_set_style_pad_left(s_chart, 50, 0);
    lv_obj_set_style_pad_right(s_chart, 50, 0);
    lv_obj_set_style_text_color(s_chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(s_chart, &lv_font_montserrat_14, LV_PART_TICKS);

    s_ser_aletas     = lv_chart_add_series(s_chart, colores[0], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_congelador = lv_chart_add_series(s_chart, colores[1], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_exterior   = lv_chart_add_series(s_chart, colores[2], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_fan        = lv_chart_add_series(s_chart, colores[3], LV_CHART_AXIS_SECONDARY_Y);

    /* Rellenar con datos del datalogger */
    for (int i = 0; i < count; i++) {
        const datalogger_entry_t *e = datalogger_get_entry(i);
        if (!e) continue;
        lv_chart_set_next_value(s_chart, s_ser_aletas,
            e->T_Aletas > -120.0f ? (int16_t)e->T_Aletas : LV_CHART_POINT_NONE);
        lv_chart_set_next_value(s_chart, s_ser_congelador,
            e->T_Congelador > -120.0f ? (int16_t)e->T_Congelador : LV_CHART_POINT_NONE);
        lv_chart_set_next_value(s_chart, s_ser_exterior,
            e->T_Exterior > -120.0f ? (int16_t)e->T_Exterior : LV_CHART_POINT_NONE);
        lv_chart_set_next_value(s_chart, s_ser_fan, e->fan_percent);
    }

    lv_chart_refresh(s_chart);

    /* Labels de hora bajo el chart de frigo */
    {
        int n = datalogger_get_count();
        {
            lv_obj_t *xlabels_cont = lv_obj_create(scr);
            lv_obj_remove_style_all(xlabels_cont);
            lv_obj_set_size(xlabels_cont, LV_HOR_RES - 20, 22);
            lv_obj_align_to(xlabels_cont, s_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
            lv_obj_set_layout(xlabels_cont, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(xlabels_cont, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(xlabels_cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            for (int i = 0; i < 5; ++i) {
                lv_obj_t *l = lv_label_create(xlabels_cont);
                lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
                lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
                if (n <= 0) {
                    lv_label_set_text(l, "--:--");
                    continue;
                }
                int idx = (n - 1) * i / 4;
                const datalogger_entry_t *e = datalogger_get_entry(idx);
                if (e) {
                    /* timestamp formato "YYYY-MM-DD HH:MM:SS" o "BOOT+HH:MM:SS"
                       Mostramos solo HH:MM (subcadena con offset) */
                    const char *ts = e->timestamp;
                    if (strncmp(ts, "BOOT", 4) == 0) {
                        /* "BOOT+HH:MM:SS" -> HH:MM */
                        char buf[8] = {0};
                        if (strlen(ts) >= 10) {
                            strncpy(buf, ts + 5, 5);
                        }
                        lv_label_set_text(l, buf);
                    } else if (strlen(ts) >= 16) {
                        /* "YYYY-MM-DD HH:MM:SS" -> HH:MM */
                        char buf[8] = {0};
                        strncpy(buf, ts + 11, 5);
                        lv_label_set_text(l, buf);
                    } else {
                        lv_label_set_text(l, ts);
                    }
                } else {
                    lv_label_set_text(l, "--:--");
                }
            }
        }
    }
}

static void frigo_swipe_cb(lv_event_t *e)
{
    ESP_LOGI("SWIPE", "Evento gesture detectado");
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir != LV_DIR_RIGHT) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    ui_show_chart_screen(ui);
}


/* --- Pantalla historico bateria --- */
static lv_obj_t *s_bh_screen = NULL;
static lv_obj_t *s_bh_chart  = NULL;

static lv_obj_t *s_bh_prev_screen = NULL;

static void bh_screen_close_cb(lv_event_t *e)
{
    lv_obj_t *scr = s_bh_screen;
    lv_obj_t *prev = s_bh_prev_screen;
    s_bh_screen = NULL;
    s_bh_chart  = NULL;
    s_bh_prev_screen = NULL;
    if (prev) lv_scr_load(prev);
    if (scr) lv_obj_del(scr);
}

void ui_show_battery_history_screen(ui_state_t *ui)
{
    (void)ui;
    if (s_bh_screen) return;

    lv_obj_t *prev = lv_scr_act();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_bh_screen = scr;

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    /* Pasamos el screen al cb para borrarlo y volver al anterior */
    lv_obj_add_event_cb(btn_close, bh_screen_close_cb, LV_EVENT_CLICKED, scr);
    /* truco: en el cb usamos user_data == scr y lv_obj_get_screen(scr) devuelve scr,
       asi que cargamos la pantalla anterior por su puntero capturado */

    /* Titulo centrado */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "HISTORICO BATERIA (24H)");
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 16);

    /* Totales: 2x2 con flex wrap */
    lv_obj_t *totals_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(totals_cont);
    lv_obj_set_size(totals_cont, LV_HOR_RES - 32, 110);
    lv_obj_align(totals_cont, LV_ALIGN_TOP_LEFT, 16, 70);
    lv_obj_set_layout(totals_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(totals_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(totals_cont, 24, 0);
    lv_obj_set_style_pad_row(totals_cont, 6, 0);

    static const uint32_t colors[BH_SRC_COUNT] = {
        0x4FC3F7, /* BM cyan */
        0xFFD54F, /* Solar amber */
        0xFF8A65, /* Orion orange */
        0xAED581, /* AC green */
    };

    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        float ch = 0, dis = 0;
        battery_history_get_totals((bh_source_t)i, &ch, &dis);
        lv_obj_t *l = lv_label_create(totals_cont);
        lv_obj_set_width(l, (LV_HOR_RES - 32 - 24) / 2);
        lv_obj_set_style_text_color(l, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(l, "%s\ncarga %.1f Ah  desc %.1f Ah",
                              battery_history_source_name((bh_source_t)i), ch, dis);
    }

    /* Chart */
    lv_obj_t *chart = lv_chart_create(scr);
    lv_obj_set_size(chart, LV_HOR_RES - 40, LV_VER_RES - 250);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(chart, 5, 8);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_chart_set_point_count(chart, BH_POINTS);
    /* Rango Y: -50A..+50A en deciamperios -> -500..+500 deci o -50000..+50000 milli */
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -40, 40);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 8, 4, 5, 1, true, 50);
    lv_obj_set_style_pad_left(chart, 50, 0);
    lv_obj_set_style_text_color(chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(chart, &lv_font_montserrat_14, LV_PART_TICKS);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    s_bh_chart = chart;

    /* Series + datos */
    bh_point_t *pts = malloc(sizeof(bh_point_t) * BH_POINTS);
    if (pts) {
        for (int i = 0; i < BH_SRC_COUNT; ++i) {
            lv_chart_series_t *ser = lv_chart_add_series(chart,
                lv_color_hex(colors[i]), LV_CHART_AXIS_PRIMARY_Y);
            int32_t old_ts = 0, new_ts = 0;
            size_t n = battery_history_get_series((bh_source_t)i, pts, &old_ts, &new_ts);
            for (size_t k = 0; k < n; ++k) {
                if (pts[k].valid) {
                    int32_t a = pts[k].milli_amps / 1000;
                    if (a >  40) a =  40;
                    if (a < -40) a = -40;
                    lv_chart_set_next_value(chart, ser, (lv_coord_t)a);
                } else {
                    lv_chart_set_next_value(chart, ser, LV_CHART_POINT_NONE);
                }
            }
        }
        free(pts);
    }
    lv_chart_refresh(chart);

    /* Labels de hora bajo el chart */
    int32_t old_ts_global = INT32_MAX, new_ts_global = INT32_MIN;
    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        int32_t ots = 0, nts = 0;
        battery_history_get_time_range((bh_source_t)i, &ots, &nts);
        if (ots > 0 && ots < old_ts_global) old_ts_global = ots;
        if (nts > new_ts_global) new_ts_global = nts;
    }
    if (old_ts_global == INT32_MAX) old_ts_global = 0;
    if (new_ts_global == INT32_MIN) new_ts_global = 0;

    lv_obj_t *xlabels_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(xlabels_cont);
    lv_obj_set_size(xlabels_cont, LV_HOR_RES - 40, 22);
    lv_obj_align_to(xlabels_cont, chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_layout(xlabels_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(xlabels_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(xlabels_cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int32_t span = new_ts_global - old_ts_global;
    if (span < 0) span = 0;
    bool have_real_time = (new_ts_global > 1704067200);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_label_create(xlabels_cont);
        lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        if (span <= 0) {
            lv_label_set_text(l, "--:--");
            continue;
        }
        int32_t ts_at = old_ts_global + (int32_t)((int64_t)span * i / 4);
        if (have_real_time) {
            time_t t = ts_at;
            struct tm tm_local;
            localtime_r(&t, &tm_local);
            lv_label_set_text_fmt(l, "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        } else {
            int32_t age_min = (new_ts_global - ts_at) / 60;
            lv_label_set_text_fmt(l, "-%dm", (int)age_min);
        }
    }

    s_bh_prev_screen = prev;
    lv_scr_load(scr);
}

static void clock_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    if (lvgl_port_lock(50)) {
        ui_show_chart_screen(ui);
        lvgl_port_unlock();
    }
}


static void ble_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_ble) return;
    int64_t now = esp_timer_get_time();
    int64_t age_ms = (now - s_last_ble_data_us) / 1000;
    /* Sin datos nunca recibidos o > 5s sin actualizacion -> gris */
    if (s_last_ble_data_us == 0 || age_ms > 5000) {
        lv_label_set_text(ui->lbl_ble, "BLE: --");
        lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x888888), 0);
    }
}

