#include "view_default_battery.h"
#include <stdlib.h>
#include <string.h>
#include "ui_format.h"

typedef struct {
    ui_device_view_t base;
    lv_obj_t *main_container;
    lv_obj_t *metrics_row;
    lv_obj_t *left_column;
    lv_obj_t *center_column;
    lv_obj_t *right_column;
    lv_obj_t *arc_container;
    lv_obj_t *soc_arc;
    lv_obj_t *soc_label;
    lv_obj_t *battery_voltage_label;
    lv_obj_t *ttg_label;
    lv_obj_t *power_consumption_label;
    
    // Left column metrics (DC/DC)
    lv_obj_t *dcdc_input_voltage_label;
    lv_obj_t *dcdc_output_voltage_label;
    lv_obj_t *dcdc_input_current_label;
    
    // Right column metrics (Solar)
    lv_obj_t *solar_power_label;
    lv_obj_t *solar_voltage_label; /* repurposed to show Charge A */
    lv_obj_t *solar_status_label;  /* new: shows solar status under charge */
    
    // Persistent device data state
    struct {
        bool has_data;
        uint16_t soc_deci_percent;
        uint16_t battery_voltage_cv;
        uint32_t ttg_minutes;
        int32_t battery_current_milli;
        uint32_t last_update_time;
    } battery_state;
    
    struct {
        bool has_data;
        uint16_t input_voltage_centi;
        uint16_t output_voltage_centi;
        uint16_t input_current_deci; // For Orion XS
        uint8_t device_state;
        victron_record_type_t device_type; // DCDC_CONVERTER or ORION_XS
        uint32_t last_update_time;
    } dcdc_state;
    
    struct {
        bool has_data;
        uint16_t pv_power_w;
        uint16_t battery_voltage_centi;
        uint16_t battery_current_deci;
        uint32_t last_update_time;
    } solar_state;
} ui_default_battery_view_t;

static void default_battery_view_update(ui_device_view_t *view, const victron_data_t *data);
static void default_battery_view_show(ui_device_view_t *view);
static void default_battery_view_hide(ui_device_view_t *view);
static void default_battery_view_destroy(ui_device_view_t *view);
static void update_display_elements(ui_default_battery_view_t *battery_view);
static uint32_t get_current_time_ms(void);

ui_device_view_t *ui_default_battery_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (ui == NULL || parent == NULL) {
        return NULL;
    }

    ui_default_battery_view_t *view = calloc(1, sizeof(*view));
    if (view == NULL) {
        return NULL;
    }

    view->base.ui = ui;
    view->base.root = lv_obj_create(parent);
    
    // Configure root container
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_outline_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 0, 0);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->base.root,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    // Create main container for the metrics layout
    view->main_container = lv_obj_create(view->base.root);
    lv_obj_set_size(view->main_container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->main_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->main_container, 0, 0);
    lv_obj_set_style_outline_width(view->main_container, 0, 0);
    lv_obj_set_style_pad_top(view->main_container, 10, 0);    // Minimal padding to account for menu
    lv_obj_set_style_pad_bottom(view->main_container, 10, 0);
    lv_obj_set_style_pad_left(view->main_container, 10, 0);
    lv_obj_set_style_pad_right(view->main_container, 10, 0);
    lv_obj_clear_flag(view->main_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->main_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->main_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    // Create horizontal row container for the three columns
    view->metrics_row = lv_obj_create(view->main_container);
    lv_obj_set_size(view->metrics_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->metrics_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->metrics_row, 0, 0);
    lv_obj_set_style_outline_width(view->metrics_row, 0, 0);
    lv_obj_set_style_pad_all(view->metrics_row, 0, 0);
    lv_obj_clear_flag(view->metrics_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->metrics_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->metrics_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create left column (DC/DC metrics)
    view->left_column = lv_obj_create(view->metrics_row);
    lv_obj_set_size(view->left_column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->left_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->left_column, 0, 0);
    lv_obj_set_style_outline_width(view->left_column, 0, 0);
    lv_obj_set_style_pad_all(view->left_column, 20, 0);
    lv_obj_clear_flag(view->left_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->left_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->left_column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create center column (SOC arc and battery info)
    view->center_column = lv_obj_create(view->metrics_row);
    lv_obj_set_size(view->center_column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->center_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->center_column, 0, 0);
    lv_obj_set_style_outline_width(view->center_column, 0, 0);
    lv_obj_set_style_pad_all(view->center_column, 20, 0);
    lv_obj_clear_flag(view->center_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->center_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->center_column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create right column (Solar metrics)
    view->right_column = lv_obj_create(view->metrics_row);
    lv_obj_set_size(view->right_column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->right_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->right_column, 0, 0);
    lv_obj_set_style_outline_width(view->right_column, 0, 0);
    lv_obj_set_style_pad_all(view->right_column, 20, 0);
    lv_obj_clear_flag(view->right_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(view->right_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->right_column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // === LEFT COLUMN: DC/DC Metrics ===
    view->dcdc_input_voltage_label = lv_label_create(view->left_column);
    lv_label_set_text(view->dcdc_input_voltage_label, "DC In:\n--V");
    lv_obj_add_style(view->dcdc_input_voltage_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->dcdc_input_voltage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(view->dcdc_input_voltage_label, 15, 0);

    view->dcdc_output_voltage_label = lv_label_create(view->left_column);
    lv_label_set_text(view->dcdc_output_voltage_label, "DC Out:\n--V");
    lv_obj_add_style(view->dcdc_output_voltage_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->dcdc_output_voltage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(view->dcdc_output_voltage_label, 15, 0);

    view->dcdc_input_current_label = lv_label_create(view->left_column);
    lv_label_set_text(view->dcdc_input_current_label, "DC In:\n--A");
    lv_obj_add_style(view->dcdc_input_current_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->dcdc_input_current_label, LV_TEXT_ALIGN_CENTER, 0);

    // === CENTER COLUMN: SOC Arc and Battery Info ===
    // Create arc container (smaller to fit with menu)
    view->arc_container = lv_obj_create(view->center_column);
    lv_obj_set_size(view->arc_container, 280, 280);
    lv_obj_set_style_bg_opa(view->arc_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->arc_container, 0, 0);
    lv_obj_set_style_outline_width(view->arc_container, 0, 0);
    lv_obj_set_style_pad_all(view->arc_container, 0, 0);
    lv_obj_clear_flag(view->arc_container, LV_OBJ_FLAG_SCROLLABLE);
    // Center the arc container horizontally in the center column with slight left offset
    lv_obj_align(view->arc_container, LV_ALIGN_CENTER, 0, 0);

    // Create SOC arc (smaller size)
    view->soc_arc = lv_arc_create(view->arc_container);
    lv_obj_set_size(view->soc_arc, 280, 280);
    lv_obj_center(view->soc_arc);
    
    // Configure arc appearance (315 degrees with 45-degree gap at bottom)
    lv_arc_set_rotation(view->soc_arc, 292); // Start from top-right to leave gap at bottom
    lv_arc_set_bg_angles(view->soc_arc, 0, 315); // 315 degrees (360 - 45)
    lv_arc_set_angles(view->soc_arc, 0, 0); // Will be updated with actual percentage
    lv_obj_remove_style(view->soc_arc, NULL, LV_PART_KNOB);   // Remove knob/handle
    lv_obj_clear_flag(view->soc_arc, LV_OBJ_FLAG_CLICKABLE);  // Make it non-interactive
    
    // Style the arc background (track)
    lv_obj_set_style_arc_width(view->soc_arc, 25, LV_PART_MAIN);
    lv_obj_set_style_arc_color(view->soc_arc, lv_color_hex(0x333333), LV_PART_MAIN);
    
    // Style the arc indicator (progress)
    lv_obj_set_style_arc_width(view->soc_arc, 25, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(view->soc_arc, lv_color_hex(0x00C851), LV_PART_INDICATOR); // Green color

    // Create SOC percentage label in the center of arc
    view->soc_label = lv_label_create(view->arc_container);
    lv_label_set_text(view->soc_label, "---%");
    lv_obj_add_style(view->soc_label, &ui->styles.medium, 0);  // Use medium instead of big font
    lv_obj_set_style_text_align(view->soc_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->soc_label, LV_ALIGN_CENTER, 0, -25);  // Move up more to make room for voltage

    // Create battery voltage label below SOC inside the arc
    view->battery_voltage_label = lv_label_create(view->arc_container);
    lv_label_set_text(view->battery_voltage_label, "--.-V");
    lv_obj_add_style(view->battery_voltage_label, &ui->styles.small, 0);  // Use smaller font
    lv_obj_set_style_text_align(view->battery_voltage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->battery_voltage_label, LV_ALIGN_CENTER, 0, 25);  // Position below SOC

    // Create time to go label (with minimal spacing)
    view->ttg_label = lv_label_create(view->center_column);
    lv_label_set_text(view->ttg_label, "TTG: --");
    lv_obj_add_style(view->ttg_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->ttg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(view->ttg_label, 10, 0);

    // Create power consumption label below TTG
    view->power_consumption_label = lv_label_create(view->center_column);
    lv_label_set_text(view->power_consumption_label, "Power: --W");
    lv_obj_add_style(view->power_consumption_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->power_consumption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(view->power_consumption_label, 10, 0);

    // === RIGHT COLUMN: Solar Metrics ===
    view->solar_power_label = lv_label_create(view->right_column);
    lv_label_set_text(view->solar_power_label, "Solar:\n--W");
    lv_obj_add_style(view->solar_power_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->solar_power_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(view->solar_power_label, 5, 0);

    // Replace 'solar batt' with 'solar charge A' (use solar_voltage_label to show charge amps)
    view->solar_voltage_label = lv_label_create(view->right_column);
    lv_label_set_text(view->solar_voltage_label, "Charge:\n--A");
    lv_obj_add_style(view->solar_voltage_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->solar_voltage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(view->solar_voltage_label, 5, 0);

    // New: Solar status label below charge
    view->solar_status_label = lv_label_create(view->right_column);
    lv_label_set_text(view->solar_status_label, "Status:\n--");
    lv_obj_add_style(view->solar_status_label, &ui->styles.small, 0);
    lv_obj_set_style_text_align(view->solar_status_label, LV_TEXT_ALIGN_CENTER, 0);

    // Initialize persistent state
    view->battery_state.has_data = false;
    view->battery_state.soc_deci_percent = 0;
    view->battery_state.battery_voltage_cv = 0;
    view->battery_state.ttg_minutes = 0xFFFFFFFF;
    view->battery_state.battery_current_milli = 0;
    view->battery_state.last_update_time = 0;
    
    view->dcdc_state.has_data = false;
    view->dcdc_state.input_voltage_centi = 0;
    view->dcdc_state.output_voltage_centi = 0;
    view->dcdc_state.input_current_deci = 0;
    view->dcdc_state.device_state = 0;
    view->dcdc_state.device_type = VICTRON_BLE_RECORD_DCDC_CONVERTER;
    view->dcdc_state.last_update_time = 0;
    
    view->solar_state.has_data = false;
    view->solar_state.pv_power_w = 0;
    view->solar_state.battery_voltage_centi = 0;
    view->solar_state.battery_current_deci = 0;
    view->solar_state.last_update_time = 0;

    // Set up function pointers
    view->base.update = default_battery_view_update;
    view->base.show = default_battery_view_show;
    view->base.hide = default_battery_view_hide;
    view->base.destroy = default_battery_view_destroy;

    return &view->base;
}

static ui_default_battery_view_t *default_battery_view_from_base(ui_device_view_t *base)
{
    return (ui_default_battery_view_t *)base;
}

static void default_battery_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_default_battery_view_t *battery_view = default_battery_view_from_base(view);
    if (battery_view == NULL) {
        return;
    }

    uint32_t current_time = get_current_time_ms();
    
    // === Process incoming data and update persistent state ===
    if (data != NULL) {
        switch (data->type) {
            case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
                const victron_record_battery_monitor_t *b = &data->record.battery;
                battery_view->battery_state.has_data = true;
                battery_view->battery_state.soc_deci_percent = b->soc_deci_percent;
                battery_view->battery_state.battery_voltage_cv = b->battery_voltage_centi;
                battery_view->battery_state.ttg_minutes = b->time_to_go_minutes;
                battery_view->battery_state.battery_current_milli = b->battery_current_milli;
                battery_view->battery_state.last_update_time = current_time;
                break;
            }
            case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
                const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
                battery_view->battery_state.has_data = true;
                battery_view->battery_state.soc_deci_percent = b->soc_deci_percent;
                battery_view->battery_state.battery_voltage_cv = b->battery_voltage_centi;
                battery_view->battery_state.ttg_minutes = b->time_to_go_min;
                battery_view->battery_state.last_update_time = current_time;
                break;
            }
            case VICTRON_BLE_RECORD_VE_BUS: {
                const victron_record_ve_bus_t *b = &data->record.vebus;
                battery_view->battery_state.has_data = true;
                battery_view->battery_state.soc_deci_percent = (uint16_t)b->soc_percent * 10; // Convert to deci-percent
                battery_view->battery_state.battery_voltage_cv = 0; // VE.Bus doesn't provide battery voltage in this record
                battery_view->battery_state.ttg_minutes = 0xFFFFFFFF; // No TTG in VE.Bus record
                battery_view->battery_state.last_update_time = current_time;
                break;
            }
            case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
                const victron_record_dcdc_converter_t *dcdc = &data->record.dcdc;
                battery_view->dcdc_state.has_data = true;
                battery_view->dcdc_state.input_voltage_centi = dcdc->input_voltage_centi;
                battery_view->dcdc_state.output_voltage_centi = dcdc->output_voltage_centi;
                battery_view->dcdc_state.input_current_deci = 0; // DC/DC converter doesn't have input current
                battery_view->dcdc_state.device_state = dcdc->device_state;
                battery_view->dcdc_state.device_type = VICTRON_BLE_RECORD_DCDC_CONVERTER;
                battery_view->dcdc_state.last_update_time = current_time;
                break;
            }
            case VICTRON_BLE_RECORD_ORION_XS: {
                const victron_record_orion_xs_t *orion = &data->record.orion;
                battery_view->dcdc_state.has_data = true;
                battery_view->dcdc_state.input_voltage_centi = orion->input_voltage_centi;
                battery_view->dcdc_state.output_voltage_centi = orion->output_voltage_centi;
                battery_view->dcdc_state.input_current_deci = orion->input_current_deci;
                battery_view->dcdc_state.device_state = orion->device_state;
                battery_view->dcdc_state.device_type = VICTRON_BLE_RECORD_ORION_XS;
                battery_view->dcdc_state.last_update_time = current_time;
                break;
            }
            case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
                const victron_record_solar_charger_t *solar = &data->record.solar;
                battery_view->solar_state.has_data = true;
                battery_view->solar_state.pv_power_w = solar->pv_power_w;
                battery_view->solar_state.battery_voltage_centi = solar->battery_voltage_centi;
                battery_view->solar_state.battery_current_deci = solar->battery_current_deci;
                battery_view->solar_state.last_update_time = current_time;
                break;
            }
            default:
                // Ignore other device types for this view
                return;
        }
    }

    // === Update the display elements based on current state ===
    update_display_elements(battery_view);
}

static void default_battery_view_show(ui_device_view_t *view)
{
    if (view && view->root) {
        lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void default_battery_view_hide(ui_device_view_t *view)
{
    if (view && view->root) {
        lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void default_battery_view_destroy(ui_device_view_t *view)
{
    if (view == NULL) {
        return;
    }
    if (view->root) {
        lv_obj_del(view->root);
        view->root = NULL;
    }
    free(view);
}

static uint32_t get_current_time_ms(void)
{
    return lv_tick_get();
}

static void update_display_elements(ui_default_battery_view_t *battery_view)
{
    if (battery_view == NULL) {
        return;
    }

    uint32_t current_time = get_current_time_ms();
    const uint32_t DATA_TIMEOUT_MS = 30000; // 30 seconds timeout

    // === Update SOC Arc and Battery Info (Center) ===
    if (battery_view->battery_state.has_data && 
        (current_time - battery_view->battery_state.last_update_time) < DATA_TIMEOUT_MS) {
        
        uint16_t soc_percent = battery_view->battery_state.soc_deci_percent / 10;
        uint16_t soc_decimal = battery_view->battery_state.soc_deci_percent % 10;
        
        // Update arc angle (0-315 degrees for 0-100% with 45-degree gap at bottom)
        int16_t arc_angle = (int16_t)((battery_view->battery_state.soc_deci_percent * 315) / 1000);
        lv_arc_set_angles(battery_view->soc_arc, 0, arc_angle);
        
        // Update arc color based on SOC level
        if (soc_percent >= 50) {
            lv_obj_set_style_arc_color(battery_view->soc_arc, lv_color_hex(0x00C851), LV_PART_INDICATOR); // Green
        } else if (soc_percent >= 25) {
            lv_obj_set_style_arc_color(battery_view->soc_arc, lv_color_hex(0xFF9800), LV_PART_INDICATOR); // Orange
        } else {
            lv_obj_set_style_arc_color(battery_view->soc_arc, lv_color_hex(0xF44336), LV_PART_INDICATOR); // Red
        }
        
        // Update SOC percentage label
        lv_label_set_text_fmt(battery_view->soc_label, "%u.%u%%", soc_percent, soc_decimal);
        
        // Update battery voltage
        if (battery_view->battery_state.battery_voltage_cv > 0) {
            uint16_t volts = battery_view->battery_state.battery_voltage_cv / 100;
            uint16_t hundredths = battery_view->battery_state.battery_voltage_cv % 100;
            lv_label_set_text_fmt(battery_view->battery_voltage_label, "%u.%02uV", volts, hundredths);
        } else {
            lv_label_set_text(battery_view->battery_voltage_label, "--.-V");
        }
        
        // Update time to go
        if (battery_view->battery_state.ttg_minutes != 0xFFFFFFFF && battery_view->battery_state.ttg_minutes > 0) {
            if (battery_view->battery_state.ttg_minutes >= 60) {
                uint32_t hours = battery_view->battery_state.ttg_minutes / 60;
                uint32_t minutes = battery_view->battery_state.ttg_minutes % 60;
                if (hours < 24) {
                    lv_label_set_text_fmt(battery_view->ttg_label, "TTG: %uh %um", (unsigned int)hours, (unsigned int)minutes);
                } else {
                    uint32_t days = hours / 24;
                    hours = hours % 24;
                    lv_label_set_text_fmt(battery_view->ttg_label, "TTG: %ud %uh", (unsigned int)days, (unsigned int)hours);
                }
            } else {
                lv_label_set_text_fmt(battery_view->ttg_label, "TTG: %um", (unsigned int)battery_view->battery_state.ttg_minutes);
            }
        } else {
            lv_label_set_text(battery_view->ttg_label, "TTG: --");
        }
        
        // Update power consumption (P = V * I)
        if (battery_view->battery_state.battery_voltage_cv > 0 && battery_view->battery_state.battery_current_milli != 0) {
            // Calculate power: voltage (centi-volts) * current (milli-amps) = milli-watts * 100
            // Convert to watts: (V_cv / 100) * (I_mA / 1000) = (V_cv * I_mA) / 100000
            int32_t power_watts = (int32_t)battery_view->battery_state.battery_voltage_cv * battery_view->battery_state.battery_current_milli / 100000;
            
            if (power_watts >= 0) {
                lv_label_set_text_fmt(battery_view->power_consumption_label, "Power: %ldW", (long)power_watts);
                lv_obj_set_style_text_color(battery_view->power_consumption_label, lv_color_hex(0x00C851), 0); // Verde
            } else {
                lv_label_set_text_fmt(battery_view->power_consumption_label, "Power: %ldW", (long)power_watts);
                lv_obj_set_style_text_color(battery_view->power_consumption_label, lv_color_hex(0xFF9800), 0); // Naranja
            }
        } else {
            lv_label_set_text(battery_view->power_consumption_label, "Power: --W");
            lv_obj_set_style_text_color(battery_view->power_consumption_label, lv_color_white(), 0); // Blanco sin datos
        }
    } else {
        // Battery data expired or not available
        lv_arc_set_angles(battery_view->soc_arc, 0, 0);
        lv_obj_set_style_arc_color(battery_view->soc_arc, lv_color_hex(0x666666), LV_PART_INDICATOR); // Gray
        lv_label_set_text(battery_view->soc_label, "---%");
        lv_label_set_text(battery_view->battery_voltage_label, "--.-V");
        lv_label_set_text(battery_view->ttg_label, "TTG: --");
        lv_label_set_text(battery_view->power_consumption_label, "Power: --W");
    }

    // === Update DC/DC Input Metrics (Left Column) ===
    if (battery_view->dcdc_state.has_data && 
        (current_time - battery_view->dcdc_state.last_update_time) < DATA_TIMEOUT_MS) {
        
        if (battery_view->dcdc_state.input_voltage_centi > 0) {
            uint16_t volts = battery_view->dcdc_state.input_voltage_centi / 100;
            uint16_t tenths = (battery_view->dcdc_state.input_voltage_centi % 100) / 10;
            lv_label_set_text_fmt(battery_view->dcdc_input_voltage_label, "DC In:\n%u.%uV", volts, tenths);
        } else {
            lv_label_set_text(battery_view->dcdc_input_voltage_label, "DC In:\n--V");
        }
        
        if (battery_view->dcdc_state.device_state != VIC_STATE_OFF && battery_view->dcdc_state.output_voltage_centi > 0) {
            uint16_t volts = battery_view->dcdc_state.output_voltage_centi / 100;
            uint16_t tenths = (battery_view->dcdc_state.output_voltage_centi % 100) / 10;
            lv_label_set_text_fmt(battery_view->dcdc_output_voltage_label, "DC Out:\n%u.%uV", volts, tenths);
        } else {
            lv_label_set_text(battery_view->dcdc_output_voltage_label, "DC Out:\n--V");
        }
        
        if (battery_view->dcdc_state.device_type == VICTRON_BLE_RECORD_ORION_XS && 
            battery_view->dcdc_state.input_current_deci > 0) {
            uint16_t amps = battery_view->dcdc_state.input_current_deci / 10;
            uint16_t tenths = battery_view->dcdc_state.input_current_deci % 10;
            lv_label_set_text_fmt(battery_view->dcdc_input_current_label, "DC In:\n%u.%uA", amps, tenths);
        } else {
            // DC/DC converter status or Orion XS without current
            if (battery_view->dcdc_state.device_state != VIC_STATE_OFF) {
                lv_label_set_text(battery_view->dcdc_input_current_label, "Status:\nActive");
            } else {
                lv_label_set_text(battery_view->dcdc_input_current_label, "Status:\nOff");
            }
        }
    } else {
        // DC/DC data expired or not available
        lv_label_set_text(battery_view->dcdc_input_voltage_label, "DC In:\n--V");
        lv_label_set_text(battery_view->dcdc_output_voltage_label, "DC Out:\n--V");
        lv_label_set_text(battery_view->dcdc_input_current_label, "DC In:\n--A");
    }

    // === Update Solar Metrics (Right Column) ===
    if (battery_view->solar_state.has_data && 
        (current_time - battery_view->solar_state.last_update_time) < DATA_TIMEOUT_MS) {
        
        // Solar Power
        if (battery_view->solar_state.pv_power_w > 0) {
            lv_label_set_text_fmt(battery_view->solar_power_label, "Solar:\n%uW", battery_view->solar_state.pv_power_w);
        } else {
            lv_label_set_text(battery_view->solar_power_label, "Solar:\n--W");
        }
        
        // Solar Charge (battery charging current) - moved into first slot
        if (battery_view->solar_state.battery_current_deci > 0) {
            uint16_t amps = battery_view->solar_state.battery_current_deci / 10;
            uint16_t tenths = battery_view->solar_state.battery_current_deci % 10;
            lv_label_set_text_fmt(battery_view->solar_voltage_label, "Charge:\n%u.%uA", amps, tenths);
        } else {
            lv_label_set_text(battery_view->solar_voltage_label, "Charge:\n--A");
        }

        // Solar status (below charge): infer from pv power / charging current
        if (battery_view->solar_state.pv_power_w > 0 || battery_view->solar_state.battery_current_deci > 0) {
            lv_label_set_text(battery_view->solar_status_label, "Status:\nCharging");
        } else {
            lv_label_set_text(battery_view->solar_status_label, "Status:\nIdle");
        }
    } else {
        // Solar data expired or not available
        lv_label_set_text(battery_view->solar_power_label, "Solar:\n--W");
        lv_label_set_text(battery_view->solar_voltage_label, "Charge:\n--A");
        lv_label_set_text(battery_view->solar_status_label, "Status:\n--");
    }

}