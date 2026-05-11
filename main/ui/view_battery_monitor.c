#include "view_battery_monitor.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ui_format.h"
#include "ui_card.h"
#include "victron_alarms.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "esp_log.h"
#include "ui.h"

static void battery_view_root_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_SHORT_CLICKED) return;
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_battery_history_screen(ui);
}

typedef struct {
    ui_device_view_t base;
    lv_obj_t *card;
    lv_obj_t *pill_state;      /* "Cargando 5.5 A" / "Descargando 3.2 A" / "Reposo" */
    lv_obj_t *arc_soc;
    lv_obj_t *m_voltage;
    lv_obj_t *m_current;
    lv_obj_t *bar_current;     /* Barra bipolar bajo m_current */
    lv_obj_t *m_power;
    lv_obj_t *m_ttg;
    lv_obj_t *m_consumed;
    lv_obj_t *m_aux;
} ui_battery_view_t;

static void battery_view_update(ui_device_view_t *view, const victron_data_t *data);
static void battery_view_show(ui_device_view_t *view);
static void battery_view_hide(ui_device_view_t *view);
static void battery_view_destroy(ui_device_view_t *view);
static void register_tap(ui_battery_view_t *view, lv_obj_t *obj, ui_state_t *ui);

ui_device_view_t *ui_battery_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (ui == NULL || parent == NULL) return NULL;

    ui_battery_view_t *view = calloc(1, sizeof(*view));
    if (view == NULL) return NULL;

    view->base.ui   = ui;
    view->base.root = lv_obj_create(parent);
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 8, 0);
    lv_obj_set_style_pad_gap(view->base.root, 8, 0);
    lv_obj_set_layout(view->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    /* ── Card "Batería" border naranja ─────────────────────────── */
    view->card = ui_card_create(view->base.root, UI_COLOR_ORANGE);

    lv_obj_t *header = ui_card_set_title_img(view->card, &icon_battery,
                                             "Batería", UI_COLOR_ORANGE);
    view->pill_state = ui_pill_create(header, "Reposo", UI_COLOR_TEXT_DIM);

    /* Body row: arc SOC izquierda (tamaño fijo) + columna métricas que
     * llena el espacio restante (flex_grow=1) y centra sus métricas. */
    lv_obj_t *body = lv_obj_create(view->card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(body, 16, 0);

    view->arc_soc = ui_arc_soc_create(body, 180);

    lv_obj_t *col = lv_obj_create(body);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);  /* ocupa todo el espacio horizontal restante */
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col, 8, 0);

    view->m_voltage = ui_metric_create_compact(col, "Tensión");
    view->m_current = ui_metric_create_compact(col, "Corriente");

    /* Barra bipolar de corriente bajo m_current.
     * Centro = 0 A, izquierda = descarga (naranja), derecha = carga (verde).
     * Rango ±50 A cubre las baterías típicas de 12 V. */
    view->bar_current = lv_bar_create(col);
    lv_obj_set_size(view->bar_current, lv_pct(95), 8);
    lv_bar_set_range(view->bar_current, -50, 50);
    lv_bar_set_mode(view->bar_current, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_value(view->bar_current, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(view->bar_current, UI_COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->bar_current, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(view->bar_current, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(view->bar_current, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(view->bar_current, UI_COLOR_TEXT_DIM, LV_PART_INDICATOR);

    view->m_power   = ui_metric_create_compact(col, "Potencia");

    /* Footer row: TTG + Consumido + Aux distribuidos en todo el ancho */
    lv_obj_t *footer = lv_obj_create(view->card);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(footer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(footer, 8, 0);
    lv_obj_set_style_pad_top(footer, 8, 0);

    view->m_ttg      = ui_metric_create_compact(footer, "Autonomía");
    view->m_consumed = ui_metric_create_compact(footer, "Consumido");
    view->m_aux      = ui_metric_create_compact(footer, "Aux");

    view->base.update  = battery_view_update;
    view->base.show    = battery_view_show;
    view->base.hide    = battery_view_hide;
    view->base.destroy = battery_view_destroy;

    /* Tap → histórico 24h. Engancha en root + card + body + footer + métricas */
    register_tap(view, view->base.root, ui);
    register_tap(view, view->card, ui);
    register_tap(view, body, ui);
    register_tap(view, footer, ui);
    register_tap(view, view->m_voltage, ui);
    register_tap(view, view->m_current, ui);
    register_tap(view, view->m_power, ui);
    register_tap(view, view->m_ttg, ui);
    register_tap(view, view->m_consumed, ui);
    register_tap(view, view->m_aux, ui);

    return &view->base;
}

static void register_tap(ui_battery_view_t *view, lv_obj_t *obj, ui_state_t *ui)
{
    (void)view;
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, battery_view_root_click_cb,
                        LV_EVENT_SHORT_CLICKED, ui);
}

static ui_battery_view_t *battery_view_from_base(ui_device_view_t *base)
{
    return (ui_battery_view_t *)base;
}

static void battery_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_battery_view_t *bat = battery_view_from_base(view);
    if (bat == NULL || data == NULL ||
        data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;

    ui_card_pulse(bat->card);

    const victron_record_battery_monitor_t *b = &data->record.battery;
    char buf[24];

    /* Tensión */
    snprintf(buf, sizeof(buf), "%u.%02u",
             b->battery_voltage_centi / 100, b->battery_voltage_centi % 100);
    ui_metric_set(bat->m_voltage, buf, "V", UI_COLOR_TEXT);

    /* Corriente — signo y color por carga/descarga */
    int current_centi = ui_round_div_signed((int)b->battery_current_milli, 10);
    int abs_c = current_centi < 0 ? -current_centi : current_centi;
    snprintf(buf, sizeof(buf), "%c%d.%02d",
             current_centi >= 0 ? '+' : '-', abs_c / 100, abs_c % 100);
    ui_metric_set(bat->m_current, buf, "A",
                  ui_color_for_current((int32_t)b->battery_current_milli));

    /* Potencia W = V * A (signo del A) */
    int64_t power_mw = (int64_t)b->battery_voltage_centi *
                       (int64_t)b->battery_current_milli / 100;
    int power_w = (int)(power_mw / 1000);
    snprintf(buf, sizeof(buf), "%+d", power_w);
    ui_metric_set(bat->m_power, buf, "W",
                  ui_color_for_current((int32_t)b->battery_current_milli));

    /* Pill: si hay alarma activa, pill rojo con la razon; si no, estado normal */
    char pill_buf[40];
    const char *alarm = victron_alarm_reason_string(b->alarm_reason);
    if (alarm) {
        snprintf(pill_buf, sizeof(pill_buf), "FALLO: %s", alarm);
        ui_pill_set(bat->pill_state, pill_buf, UI_COLOR_RED);
        goto skip_state_pill;
    }
    int abs_a_pill = abs_c;  /* en centiamperios */
    if (b->battery_current_milli > 50) {
        snprintf(pill_buf, sizeof(pill_buf), "Cargando %d.%d A",
                 abs_a_pill / 100, (abs_a_pill / 10) % 10);
        ui_pill_set(bat->pill_state, pill_buf, UI_COLOR_GREEN);
    } else if (b->battery_current_milli < -50) {
        snprintf(pill_buf, sizeof(pill_buf), "Descargando %d.%d A",
                 abs_a_pill / 100, (abs_a_pill / 10) % 10);
        ui_pill_set(bat->pill_state, pill_buf, UI_COLOR_ORANGE);
    } else {
        ui_pill_set(bat->pill_state, "Reposo", UI_COLOR_TEXT_DIM);
    }
skip_state_pill:

    /* Barra bipolar de corriente: valor en A, color según signo */
    if (bat->bar_current) {
        int amps_int = (int)(b->battery_current_milli / 1000);
        if (amps_int > 50)  amps_int = 50;
        if (amps_int < -50) amps_int = -50;
        lv_bar_set_value(bat->bar_current, amps_int, LV_ANIM_ON);
        lv_color_t bar_col = (b->battery_current_milli > 0) ? UI_COLOR_GREEN
                           : (b->battery_current_milli < 0) ? UI_COLOR_ORANGE
                           : UI_COLOR_TEXT_DIM;
        lv_obj_set_style_bg_color(bat->bar_current, bar_col, LV_PART_INDICATOR);
    }

    /* Arc SOC + voltaje */
    ui_arc_soc_set(bat->arc_soc, b->soc_deci_percent, b->battery_voltage_centi);

    /* Footer: TTG */
    if (b->time_to_go_minutes == 0xFFFF) {
        ui_metric_set(bat->m_ttg, "--", "", UI_COLOR_TEXT);
    } else {
        snprintf(buf, sizeof(buf), "%uh %02um",
                 b->time_to_go_minutes / 60, b->time_to_go_minutes % 60);
        ui_metric_set(bat->m_ttg, buf, "", UI_COLOR_TEXT);
    }

    /* Footer: Consumido */
    int consumed_deci = (int)b->consumed_ah_deci;
    int abs_cons = consumed_deci < 0 ? -consumed_deci : consumed_deci;
    snprintf(buf, sizeof(buf), "%c%d.%d",
             consumed_deci < 0 ? '-' : '+',
             abs_cons / 10, abs_cons % 10);
    ui_metric_set(bat->m_consumed, buf, "Ah",
                  consumed_deci < 0 ? UI_COLOR_ORANGE : UI_COLOR_TEXT);

    /* Footer: Aux (temperatura / mid voltage / aux voltage) */
    char aux_buf[32];
    ui_format_aux_value(b->aux_input, b->aux_value, aux_buf, sizeof(aux_buf));
    ui_metric_set(bat->m_aux, aux_buf, "", UI_COLOR_TEXT);
}

static void battery_view_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void battery_view_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void battery_view_destroy(ui_device_view_t *view)
{
    if (view == NULL) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}
