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
#include <time.h>

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
    /* Fuente grande para los tabs */
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(ui->tabview);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_28, 0);
    ui->tab_live  = lv_tabview_add_tab(ui->tabview, "Live");
    ui->tab_settings = lv_tabview_add_tab(ui->tabview, "Settings");
    ui->tab_frigo = lv_tabview_add_tab(ui->tabview, "Frigo");

    ui->tab_settings_index = lv_obj_get_index(ui->tab_settings);
    ui->tab_frigo_index = lv_obj_get_index(ui->tab_frigo);

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

    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tab_frigo, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_frigo, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_frigo, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

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
    /* TODO: ui_frigo_panel_init(ui); */

    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);
    lv_timer_create(clock_timer_cb, 60000, ui);
    clock_timer_cb(NULL);
    lvgl_port_unlock();
}

void ui_on_panel_data(const victron_data_t *d) {
    if (d == NULL) {
        return;
    }

    ui_state_t *ui = &g_ui;

    lvgl_port_lock(0);

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

    struct tm t;
    if (rtc_get_time(&t) == ESP_OK && t.tm_year > 100) {
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
