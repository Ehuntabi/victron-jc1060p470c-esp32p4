#include "view_solar_charger.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ui_format.h"
#include "ui_card.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"

typedef struct {
    ui_device_view_t base;
    /* Card Solar */
    lv_obj_t *card_solar;
    lv_obj_t *pill_state;
    lv_obj_t *m_pv_power;
    lv_obj_t *m_yield;
    /* Card Salida a batería */
    lv_obj_t *card_out;
    lv_obj_t *m_voltage;
    lv_obj_t *m_current;
    lv_obj_t *m_charge_w;
    lv_obj_t *pill_error;  /* solo visible cuando charger_error != 0 */
} ui_solar_view_t;

static void solar_view_update(ui_device_view_t *view, const victron_data_t *data);
static void solar_view_show(ui_device_view_t *view);
static void solar_view_hide(ui_device_view_t *view);
static void solar_view_destroy(ui_device_view_t *view);

static const char *solar_error_string(uint8_t code);
static const char *solar_state_string(uint8_t state);

ui_device_view_t *ui_solar_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (!ui || !parent) return NULL;

    ui_solar_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    view->base.ui   = ui;
    view->base.root = lv_obj_create(parent);
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 12, 0);
    lv_obj_set_style_pad_gap(view->base.root, 12, 0);
    lv_obj_set_layout(view->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    /* ── Card Solar (verde) ────────────────────────────────────── */
    view->card_solar = ui_card_create(view->base.root, UI_COLOR_GREEN);
    lv_obj_t *header_solar = ui_card_set_title_img(view->card_solar, &icon_solar,
                                                   "Solar", UI_COLOR_GREEN);
    view->pill_state = ui_pill_create(header_solar, "-", UI_COLOR_TEXT_DIM);

    lv_obj_t *body_solar = lv_obj_create(view->card_solar);
    lv_obj_remove_style_all(body_solar);
    lv_obj_set_size(body_solar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(body_solar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body_solar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_solar, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(body_solar, 16, 0);

    view->m_pv_power = ui_metric_create_large(body_solar, "Potencia PV");
    view->m_yield    = ui_metric_create_large(body_solar, "Hoy");

    /* ── Card Salida a batería (naranja) ───────────────────────── */
    view->card_out = ui_card_create(view->base.root, UI_COLOR_ORANGE);
    lv_obj_t *header_out = ui_card_set_title_img(view->card_out, &icon_battery,
                                                 "Salida a batería",
                                                 UI_COLOR_ORANGE);
    view->pill_error = ui_pill_create(header_out, "OK", UI_COLOR_GREEN);
    lv_obj_add_flag(view->pill_error, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *body_out = lv_obj_create(view->card_out);
    lv_obj_remove_style_all(body_out);
    lv_obj_set_size(body_out, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(body_out, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body_out, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_out, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(body_out, 16, 0);

    view->m_voltage  = ui_metric_create_large(body_out, "Tensión");
    view->m_current  = ui_metric_create_large(body_out, "Corriente");
    view->m_charge_w = ui_metric_create_large(body_out, "Potencia carga");

    view->base.update  = solar_view_update;
    view->base.show    = solar_view_show;
    view->base.hide    = solar_view_hide;
    view->base.destroy = solar_view_destroy;

    return &view->base;
}

static ui_solar_view_t *solar_view_from_base(ui_device_view_t *base)
{
    return (ui_solar_view_t *)base;
}

static void solar_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_solar_view_t *solar = solar_view_from_base(view);
    if (!solar || !data || data->type != VICTRON_BLE_RECORD_SOLAR_CHARGER) return;

    const victron_record_solar_charger_t *s = &data->record.solar;
    char buf[24];

    /* Pill estado del cargador */
    uint8_t st = s->device_state;
    lv_color_t pill_bg = UI_COLOR_TEXT_DIM;
    if (st == 3) pill_bg = UI_COLOR_ORANGE;        /* Bulk */
    else if (st == 4) pill_bg = UI_COLOR_YELLOW;   /* Absorción */
    else if (st == 5) pill_bg = UI_COLOR_GREEN;    /* Float */
    else if (st == 6) pill_bg = UI_COLOR_GREEN;    /* Storage */
    else if (st == 2) pill_bg = UI_COLOR_RED;      /* Fault */
    ui_pill_set(solar->pill_state, solar_state_string(st), pill_bg);

    /* Card Solar: PV W (grande, verde si > 0) */
    snprintf(buf, sizeof(buf), "%u", (unsigned)s->pv_power_w);
    ui_metric_set(solar->m_pv_power, buf, "W",
                  s->pv_power_w > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);

    /* Yield hoy: en kWh si ≥ 1, en Wh si menor */
    unsigned long yield_wh = (unsigned long)s->yield_today_centikwh * 10UL;
    if (yield_wh >= 1000) {
        snprintf(buf, sizeof(buf), "%lu.%01lu",
                 yield_wh / 1000, (yield_wh % 1000) / 100);
        ui_metric_set(solar->m_yield, buf, "kWh", UI_COLOR_TEXT);
    } else {
        snprintf(buf, sizeof(buf), "%lu", yield_wh);
        ui_metric_set(solar->m_yield, buf, "Wh", UI_COLOR_TEXT);
    }

    /* Card Salida: tensión batería */
    snprintf(buf, sizeof(buf), "%u.%02u",
             s->battery_voltage_centi / 100,
             s->battery_voltage_centi % 100);
    ui_metric_set(solar->m_voltage, buf, "V", UI_COLOR_TEXT);

    /* Corriente — verde si carga */
    int cur_deci = s->battery_current_deci;
    int abs_c = cur_deci < 0 ? -cur_deci : cur_deci;
    snprintf(buf, sizeof(buf), "%c%d.%d",
             cur_deci >= 0 ? '+' : '-', abs_c / 10, abs_c % 10);
    ui_metric_set(solar->m_current, buf, "A",
                  cur_deci > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);

    /* Potencia de carga = V × I_bat */
    long charge_w = ((long)s->battery_voltage_centi *
                     (long)s->battery_current_deci) / 1000L;
    snprintf(buf, sizeof(buf), "%ld", charge_w);
    ui_metric_set(solar->m_charge_w, buf, "W",
                  charge_w > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);

    /* Pill de error: visible solo si charger_error != 0 */
    if (s->charger_error != 0) {
        ui_pill_set(solar->pill_error, solar_error_string(s->charger_error),
                    UI_COLOR_RED);
        lv_obj_clear_flag(solar->pill_error, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(solar->pill_error, LV_OBJ_FLAG_HIDDEN);
    }
}

static void solar_view_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void solar_view_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void solar_view_destroy(ui_device_view_t *view)
{
    if (!view) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}

/* ── Strings ─────────────────────────────────────────────────────── */
static const char *solar_error_string(uint8_t e)
{
    switch (e) {
    case 0:   return "OK";
    case 1:   return "Temp bat. alta";
    case 2:   return "V bat. alto";
    case 3:
    case 4:   return "Sensor T remoto";
    case 5:   return "Sensor T remoto perdido";
    case 6:
    case 7:   return "Sensor V remoto";
    case 8:   return "Sensor V remoto perdido";
    case 11:  return "Ripple V alto";
    case 14:  return "Batería fría";
    case 17:  return "Sobrecal. controlador";
    case 18:  return "Sobrecorriente";
    case 20:  return "Bulk excedido";
    case 21:  return "Sensor I rango";
    case 24:  return "Fallo ventilador";
    case 26:  return "Terminal sobrecal.";
    case 27:  return "Cortocircuito bat.";
    case 28:  return "HW etapa potencia";
    case 33:  return "Sobre-V PV";
    case 34:  return "Sobre-I PV";
    case 35:  return "Sobre-P PV";
    case 38:
    case 39:  return "PV cortocircuito";
    default:  return "Error";
    }
}

static const char *solar_state_string(uint8_t s)
{
    switch (s) {
    case 0:  return "Apagado";
    case 1:  return "Bajo consumo";
    case 2:  return "Fallo";
    case 3:  return "Bulk";
    case 4:  return "Absorción";
    case 5:  return "Float";
    case 6:  return "Storage";
    case 7:  return "Eq. manual";
    case 8:  return "Eq. auto";
    case 9:  return "Inversor";
    case 10: return "Fuente";
    case 11: return "Iniciando";
    default: return "-";
    }
}
