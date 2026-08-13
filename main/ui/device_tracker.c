/* device_tracker.c - ver device_tracker.h */
#include "ui/device_tracker.h"
#include "ui.h"
#include "ui/settings_panel.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "device_tracker";

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

void device_tracker_init(ui_state_t *ui)
{
    ui->device_timeout_timer = lv_timer_create(ui_check_device_timeouts, 10000, ui);
}

void ui_update_device_activity(ui_state_t *ui, const char *mac_address)
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

void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size)
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

void ui_mark_device_offline(const char *mac_address)
{
    if (mac_address == NULL) {
        return;
    }

    ui_state_t *ui = ui_get_state();

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
    ui_state_t *ui = ui_get_state();
    ESP_LOGI(TAG, "Refreshing Victron device list in settings panel");
    if (lvgl_port_lock(200)) {
        ui_settings_panel_refresh_victron_devices(ui);
        lvgl_port_unlock();
    }
}
