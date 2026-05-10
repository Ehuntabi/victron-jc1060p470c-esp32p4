#include "view_default_battery.h"
#include "ui.h"
#include "ui_card.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ui_device_view_t base;
    /* Cards */
    lv_obj_t *card_dcdc;
    lv_obj_t *card_battery;
    lv_obj_t *card_solar;
    /* DCDC */
    lv_obj_t *m_dc_in_v;
    lv_obj_t *m_dc_out_v;
    lv_obj_t *m_dc_status;   /* DC In A para Orion XS, o Status para DCDC */
    /* Battery (centro) */
    lv_obj_t *arc_soc;
    lv_obj_t *m_ttg;
    lv_obj_t *m_current;
    lv_obj_t *bar_current;
    /* Solar */
    lv_obj_t *m_solar_power;
    lv_obj_t *m_solar_charge;
    lv_obj_t *pill_solar_state;

    /* State-store con timestamps */
    struct {
        bool has_data;
        uint16_t soc_deci_percent;
        uint16_t battery_voltage_cv;
        uint32_t ttg_minutes;
        int32_t  battery_current_milli;
        uint32_t last_update_time;
    } battery_state;

    struct {
        bool has_data;
        uint16_t input_voltage_centi;
        uint16_t output_voltage_centi;
        uint16_t input_current_deci;
        uint8_t  device_state;
        victron_record_type_t device_type;
        uint32_t last_update_time;
    } dcdc_state;

    struct {
        bool has_data;
        uint16_t pv_power_w;
        uint16_t battery_voltage_centi;
        int16_t  battery_current_deci;
        uint32_t last_update_time;
    } solar_state;
} ui_default_battery_view_t;

static void default_battery_view_update(ui_device_view_t *view, const victron_data_t *data);
static void default_battery_view_show(ui_device_view_t *view);
static void default_battery_view_hide(ui_device_view_t *view);
static void default_battery_view_destroy(ui_device_view_t *view);
static void update_display_elements(ui_default_battery_view_t *bv);
static uint32_t get_current_time_ms(void) { return lv_tick_get(); }

ui_device_view_t *ui_default_battery_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (!ui || !parent) return NULL;

    ui_default_battery_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    view->base.ui = ui;
    view->base.root = lv_obj_create(parent);
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 12, 0);
    lv_obj_set_style_pad_gap(view->base.root, 12, 0);
    lv_obj_set_layout(view->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view->base.root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->base.root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    /* Cards con altura fija → repartir hijos verticalmente con SPACE_AROUND
     * para usar todo el alto disponible y centrado horizontal. */

    /* ── Card DCDC (cyan) ───────────────────────────────────────── */
    view->card_dcdc = ui_card_create(view->base.root, UI_COLOR_CYAN);
    lv_obj_set_width(view->card_dcdc, lv_pct(31));
    lv_obj_set_height(view->card_dcdc, lv_pct(100));
    lv_obj_set_flex_align(view->card_dcdc, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_card_set_title_img(view->card_dcdc, &icon_dcdc, "DC/DC", UI_COLOR_CYAN);
    view->m_dc_in_v   = ui_metric_create_compact(view->card_dcdc,
                                                 LV_SYMBOL_BATTERY_FULL " Bat. motor");
    view->m_dc_out_v  = ui_metric_create_compact(view->card_dcdc, "Salida");
    view->m_dc_status = ui_metric_create_compact(view->card_dcdc, "Estado");

    /* ── Card Batería (naranja, centro) ─────────────────────────── */
    view->card_battery = ui_card_create(view->base.root, UI_COLOR_ORANGE);
    lv_obj_set_width(view->card_battery, lv_pct(34));
    lv_obj_set_height(view->card_battery, lv_pct(100));
    lv_obj_set_flex_align(view->card_battery, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_card_set_title_img(view->card_battery, &icon_battery,
                          "Batería", UI_COLOR_ORANGE);
    view->arc_soc = ui_arc_soc_create(view->card_battery, 180);
    view->m_ttg     = ui_metric_create_compact(view->card_battery, "Autonomía");
    view->m_current = ui_metric_create_compact(view->card_battery, "Corriente");

    /* Barra bipolar de corriente bajo m_current (rango ±50 A, simétrica) */
    view->bar_current = lv_bar_create(view->card_battery);
    lv_obj_set_size(view->bar_current, lv_pct(85), 10);
    lv_bar_set_range(view->bar_current, -50, 50);
    lv_bar_set_mode(view->bar_current, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_value(view->bar_current, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(view->bar_current, UI_COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->bar_current, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(view->bar_current, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(view->bar_current, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(view->bar_current, UI_COLOR_TEXT_DIM, LV_PART_INDICATOR);

    /* ── Card Solar (verde) ─────────────────────────────────────── */
    view->card_solar = ui_card_create(view->base.root, UI_COLOR_GREEN);
    lv_obj_set_width(view->card_solar, lv_pct(31));
    lv_obj_set_height(view->card_solar, lv_pct(100));
    lv_obj_set_flex_align(view->card_solar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *header_solar = ui_card_set_title_img(view->card_solar, &icon_solar,
                                                   "Solar", UI_COLOR_GREEN);
    view->pill_solar_state = ui_pill_create(header_solar, "-", UI_COLOR_TEXT_DIM);
    view->m_solar_power  = ui_metric_create_compact(view->card_solar, "Potencia PV");
    view->m_solar_charge = ui_metric_create_compact(view->card_solar, "Carga");

    view->battery_state.ttg_minutes  = 0xFFFFFFFF;
    view->dcdc_state.device_type     = VICTRON_BLE_RECORD_DCDC_CONVERTER;

    view->base.update  = default_battery_view_update;
    view->base.show    = default_battery_view_show;
    view->base.hide    = default_battery_view_hide;
    view->base.destroy = default_battery_view_destroy;

    return &view->base;
}

static ui_default_battery_view_t *from_base(ui_device_view_t *base)
{
    return (ui_default_battery_view_t *)base;
}

static void default_battery_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_default_battery_view_t *bv = from_base(view);
    if (!bv) return;
    uint32_t now = get_current_time_ms();

    if (data) {
        switch (data->type) {
            case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
                const victron_record_battery_monitor_t *b = &data->record.battery;
                bv->battery_state.has_data = true;
                bv->battery_state.soc_deci_percent = b->soc_deci_percent;
                bv->battery_state.battery_voltage_cv = b->battery_voltage_centi;
                bv->battery_state.ttg_minutes = b->time_to_go_minutes;
                bv->battery_state.battery_current_milli = b->battery_current_milli;
                bv->battery_state.last_update_time = now;
                break;
            }
            case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
                const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
                bv->battery_state.has_data = true;
                bv->battery_state.soc_deci_percent = b->soc_deci_percent;
                bv->battery_state.battery_voltage_cv = b->battery_voltage_centi;
                bv->battery_state.ttg_minutes = b->time_to_go_min;
                bv->battery_state.last_update_time = now;
                break;
            }
            case VICTRON_BLE_RECORD_VE_BUS: {
                const victron_record_ve_bus_t *b = &data->record.vebus;
                bv->battery_state.has_data = true;
                bv->battery_state.soc_deci_percent = (uint16_t)b->soc_percent * 10;
                bv->battery_state.last_update_time = now;
                break;
            }
            case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
                const victron_record_dcdc_converter_t *d = &data->record.dcdc;
                bv->dcdc_state.has_data = true;
                bv->dcdc_state.input_voltage_centi  = d->input_voltage_centi;
                bv->dcdc_state.output_voltage_centi = d->output_voltage_centi;
                bv->dcdc_state.input_current_deci   = 0;
                bv->dcdc_state.device_state         = d->device_state;
                bv->dcdc_state.device_type          = VICTRON_BLE_RECORD_DCDC_CONVERTER;
                bv->dcdc_state.last_update_time     = now;
                break;
            }
            case VICTRON_BLE_RECORD_ORION_XS: {
                const victron_record_orion_xs_t *o = &data->record.orion;
                bv->dcdc_state.has_data = true;
                bv->dcdc_state.input_voltage_centi  = o->input_voltage_centi;
                bv->dcdc_state.output_voltage_centi = o->output_voltage_centi;
                bv->dcdc_state.input_current_deci   = o->input_current_deci;
                bv->dcdc_state.device_state         = o->device_state;
                bv->dcdc_state.device_type          = VICTRON_BLE_RECORD_ORION_XS;
                bv->dcdc_state.last_update_time     = now;
                break;
            }
            case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
                const victron_record_solar_charger_t *s = &data->record.solar;
                bv->solar_state.has_data = true;
                bv->solar_state.pv_power_w = s->pv_power_w;
                bv->solar_state.battery_voltage_centi = s->battery_voltage_centi;
                bv->solar_state.battery_current_deci = s->battery_current_deci;
                bv->solar_state.last_update_time = now;
                break;
            }
            default: return;
        }
    }

    update_display_elements(bv);
}

static void default_battery_view_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void default_battery_view_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void default_battery_view_destroy(ui_device_view_t *view)
{
    if (!view) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}

static void update_display_elements(ui_default_battery_view_t *bv)
{
    if (!bv) return;
    uint32_t now = get_current_time_ms();
    const uint32_t TIMEOUT_MS = 30000;
    char buf[24];

    /* ── Centro: Batería ────────────────────────────────────────── */
    bool bat_fresh = bv->battery_state.has_data &&
                     (now - bv->battery_state.last_update_time) < TIMEOUT_MS;
    if (bat_fresh) {
        ui_arc_soc_set(bv->arc_soc,
                       bv->battery_state.soc_deci_percent,
                       bv->battery_state.battery_voltage_cv);
        if (bv->battery_state.ttg_minutes != 0xFFFFFFFF &&
            bv->battery_state.ttg_minutes > 0) {
            uint32_t ttg = bv->battery_state.ttg_minutes;
            if (ttg >= 60) {
                snprintf(buf, sizeof(buf), "%uh %02um",
                         (unsigned)(ttg / 60), (unsigned)(ttg % 60));
            } else {
                snprintf(buf, sizeof(buf), "%um", (unsigned)ttg);
            }
            ui_metric_set(bv->m_ttg, buf, "", UI_COLOR_TEXT);
        } else {
            ui_metric_set(bv->m_ttg, "--", "", UI_COLOR_TEXT);
        }
        int32_t mi = bv->battery_state.battery_current_milli;
        int abs_a_centi = mi < 0 ? -mi/10 : mi/10;
        snprintf(buf, sizeof(buf), "%c%d.%02d",
                 mi >= 0 ? '+' : '-', abs_a_centi/100, abs_a_centi%100);
        ui_metric_set(bv->m_current, buf, "A", ui_color_for_current(mi));
        if (bv->bar_current) {
            int amps_int = (int)(mi / 1000);
            if (amps_int > 50)  amps_int = 50;
            if (amps_int < -50) amps_int = -50;
            lv_bar_set_value(bv->bar_current, amps_int, LV_ANIM_ON);
            lv_color_t col = (mi > 0) ? UI_COLOR_GREEN
                           : (mi < 0) ? UI_COLOR_ORANGE
                           : UI_COLOR_TEXT_DIM;
            lv_obj_set_style_bg_color(bv->bar_current, col, LV_PART_INDICATOR);
        }
    } else {
        ui_arc_soc_set(bv->arc_soc, 0xFFFF, 0);
        ui_metric_set(bv->m_ttg, "--", "", UI_COLOR_TEXT);
        ui_metric_set(bv->m_current, "--", "", UI_COLOR_TEXT);
        if (bv->bar_current) {
            lv_bar_set_value(bv->bar_current, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bv->bar_current, UI_COLOR_TEXT_DIM, LV_PART_INDICATOR);
        }
    }

    /* ── Izquierda: DCDC ────────────────────────────────────────── */
    bool dc_fresh = bv->dcdc_state.has_data &&
                    (now - bv->dcdc_state.last_update_time) < TIMEOUT_MS;
    if (dc_fresh) {
        if (bv->dcdc_state.input_voltage_centi > 0) {
            uint16_t vin = bv->dcdc_state.input_voltage_centi;
            /* Detectar alternador encendido por umbral según sistema:
             * 12 V → ≥13.3 V indica carga; 24 V → ≥26.6 V. */
            bool alt_on = (vin >= 2660) ||
                          (vin >= 1330 && vin < 1800);
            snprintf(buf, sizeof(buf), "%u.%02u", vin / 100, vin % 100);
            ui_metric_set(bv->m_dc_in_v, buf, "V",
                          alt_on ? UI_COLOR_GREEN : UI_COLOR_TEXT);
            ui_metric_set_label(bv->m_dc_in_v,
                                alt_on ? LV_SYMBOL_CHARGE " Alternador"
                                       : LV_SYMBOL_BATTERY_FULL " Bat. motor",
                                alt_on ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
        } else {
            ui_metric_set(bv->m_dc_in_v, "--", "", UI_COLOR_TEXT);
            ui_metric_set_label(bv->m_dc_in_v,
                                LV_SYMBOL_BATTERY_FULL " Bat. motor",
                                UI_COLOR_TEXT_DIM);
        }
        if (bv->dcdc_state.output_voltage_centi > 0 &&
            bv->dcdc_state.device_state != VIC_STATE_OFF) {
            snprintf(buf, sizeof(buf), "%u.%02u",
                     bv->dcdc_state.output_voltage_centi / 100,
                     bv->dcdc_state.output_voltage_centi % 100);
            ui_metric_set(bv->m_dc_out_v, buf, "V", UI_COLOR_TEXT);
        } else {
            ui_metric_set(bv->m_dc_out_v, "--", "", UI_COLOR_TEXT);
        }
        /* 3er metric: corriente entrada (Orion XS) o estado activo/off */
        if (bv->dcdc_state.device_type == VICTRON_BLE_RECORD_ORION_XS &&
            bv->dcdc_state.input_current_deci > 0) {
            snprintf(buf, sizeof(buf), "%u.%u",
                     bv->dcdc_state.input_current_deci / 10,
                     bv->dcdc_state.input_current_deci % 10);
            ui_metric_set(bv->m_dc_status, buf, "A In", UI_COLOR_TEXT);
        } else {
            const char *st = (bv->dcdc_state.device_state != VIC_STATE_OFF)
                ? "Activo" : "Apagado";
            lv_color_t col = (bv->dcdc_state.device_state != VIC_STATE_OFF)
                ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM;
            ui_metric_set(bv->m_dc_status, st, "", col);
        }
    } else {
        ui_metric_set(bv->m_dc_in_v,   "--", "", UI_COLOR_TEXT);
        ui_metric_set(bv->m_dc_out_v,  "--", "", UI_COLOR_TEXT);
        ui_metric_set(bv->m_dc_status, "--", "", UI_COLOR_TEXT);
    }

    /* ── Derecha: Solar ─────────────────────────────────────────── */
    bool solar_fresh = bv->solar_state.has_data &&
                       (now - bv->solar_state.last_update_time) < TIMEOUT_MS;
    if (solar_fresh) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)bv->solar_state.pv_power_w);
        ui_metric_set(bv->m_solar_power, buf, "W",
                      bv->solar_state.pv_power_w > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
        if (bv->solar_state.battery_current_deci > 0) {
            snprintf(buf, sizeof(buf), "%u.%u",
                     bv->solar_state.battery_current_deci / 10,
                     bv->solar_state.battery_current_deci % 10);
            ui_metric_set(bv->m_solar_charge, buf, "A", UI_COLOR_GREEN);
        } else {
            ui_metric_set(bv->m_solar_charge, "0.0", "A", UI_COLOR_TEXT_DIM);
        }
        if (bv->solar_state.pv_power_w > 0 || bv->solar_state.battery_current_deci > 0) {
            ui_pill_set(bv->pill_solar_state, "Cargando", UI_COLOR_GREEN);
        } else {
            ui_pill_set(bv->pill_solar_state, "Reposo", UI_COLOR_TEXT_DIM);
        }
    } else {
        ui_metric_set(bv->m_solar_power,  "--", "", UI_COLOR_TEXT);
        ui_metric_set(bv->m_solar_charge, "--", "", UI_COLOR_TEXT);
        ui_pill_set(bv->pill_solar_state, "-", UI_COLOR_TEXT_DIM);
    }
}
