#include "view_overview.h"
#include "ui_card.h"
#include "energy_today.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "ui.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ui_device_view_t base;
    /* Cards */
    lv_obj_t *card_solar;
    lv_obj_t *m_solar_w;
    /* Flecha + label de potencia Solar→Bat */
    lv_obj_t *arrow_solar;
    lv_obj_t *flow_solar;
    /* Card central Bateria */
    lv_obj_t *card_bat;
    lv_obj_t *arc_soc;
    lv_obj_t *m_bat_current;
    /* Flecha + label de potencia Bat→Loads */
    lv_obj_t *arrow_loads;
    lv_obj_t *flow_loads;
    /* Card Loads */
    lv_obj_t *card_loads;
    lv_obj_t *m_loads_w;
    /* Footer */
    lv_obj_t *m_ttg;
    lv_obj_t *lbl_energy;   /* "PV X.X kWh   Loads Y.Y kWh" */

    /* State-store consolidado por tipo */
    struct {
        bool has_data;
        uint16_t soc_deci;
        uint16_t voltage_centi;
        int32_t current_milli;
        uint32_t ttg_min;
        uint32_t last_update_ms;
    } bat;
    struct {
        bool has_data;
        uint16_t pv_w;
        int16_t  load_current_deci;
        uint16_t voltage_centi;
        uint32_t last_update_ms;
    } solar;
} ui_overview_view_t;

static void overview_update(ui_device_view_t *view, const victron_data_t *data);
static void overview_show(ui_device_view_t *view);
static void overview_hide(ui_device_view_t *view);
static void overview_destroy(ui_device_view_t *view);
static void overview_render(ui_overview_view_t *ov);
static uint32_t now_ms(void) { return lv_tick_get(); }

static lv_obj_t *create_node_card(lv_obj_t *parent, const lv_img_dsc_t *img,
                                  const char *title, lv_color_t accent,
                                  lv_obj_t **out_metric)
{
    lv_obj_t *card = ui_card_create(parent, accent);
    /* Header con icono raster + metric debajo. El caller fija altura/grow */
    ui_card_set_title_img(card, img, title, accent);
    if (out_metric) {
        *out_metric = ui_metric_create_compact(card, "");
    }
    return card;
}

/* Columna vertical (flecha derecha + label de potencia debajo) que se inserta
 * como separador entre las cards en el layout horizontal del Overview. */
static lv_obj_t *create_arrow_label(lv_obj_t *parent, lv_obj_t **out_flow)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col, 4, 0);

    lv_obj_t *arrow = lv_label_create(col);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);

    lv_obj_t *flow = lv_label_create(col);
    lv_obj_set_style_text_font(flow, &lv_font_montserrat_14_es, 0);
    lv_obj_set_style_text_color(flow, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(flow, "--");
    if (out_flow) *out_flow = flow;
    return arrow;
}

ui_device_view_t *ui_overview_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (!ui || !parent) return NULL;
    ui_overview_view_t *ov = calloc(1, sizeof(*ov));
    if (!ov) return NULL;

    ov->base.ui = ui;
    ov->base.root = lv_obj_create(parent);
    lv_obj_set_size(ov->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(ov->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov->base.root, 0, 0);
    lv_obj_set_style_pad_all(ov->base.root, 8, 0);
    lv_obj_set_style_pad_gap(ov->base.root, 8, 0);
    lv_obj_set_layout(ov->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ov->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov->base.root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ov->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN);

    /* ── Fila principal: Solar | flow | Bat | flow | Loads ───────── */
    lv_obj_t *row = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Card Solar (verde) lateral */
    ov->card_solar = create_node_card(row, &icon_solar,
                                      "Solar", UI_COLOR_GREEN, &ov->m_solar_w);
    lv_obj_set_height(ov->card_solar, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_solar, 2);
    ui_metric_set(ov->m_solar_w, "--", "W", UI_COLOR_TEXT);

    /* Flecha Solar→Bat */
    ov->arrow_solar = create_arrow_label(row, &ov->flow_solar);

    /* Card Bateria (naranja, central, más grande) */
    ov->card_bat = ui_card_create(row, UI_COLOR_ORANGE);
    lv_obj_set_height(ov->card_bat, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_bat, 3);
    ui_card_set_title_img(ov->card_bat, &icon_battery,
                          "Batería", UI_COLOR_ORANGE);
    ov->arc_soc = ui_arc_soc_create(ov->card_bat, 200);
    ov->m_bat_current = ui_metric_create_compact(ov->card_bat, "Corriente");

    /* Flecha Bat→Loads */
    ov->arrow_loads = create_arrow_label(row, &ov->flow_loads);

    /* Card Loads (cyan) lateral */
    ov->card_loads = create_node_card(row, &icon_home,
                                      "Cargas", UI_COLOR_CYAN, &ov->m_loads_w);
    lv_obj_set_height(ov->card_loads, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_loads, 2);
    ui_metric_set(ov->m_loads_w, "--", "W", UI_COLOR_TEXT);

    /* Footer: TTG (compacto, fuera de la fila) */
    ov->m_ttg = ui_metric_create_compact(ov->base.root, "Autonomía");

    /* Sub-footer: energía acumulada del día (PV + cargas) */
    ov->lbl_energy = lv_label_create(ov->base.root);
    lv_obj_set_style_text_font(ov->lbl_energy, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ov->lbl_energy, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(ov->lbl_energy, "PV --   Cargas --");

    /* Defaults */
    ov->bat.ttg_min = 0xFFFFFFFF;

    ov->base.update  = overview_update;
    ov->base.show    = overview_show;
    ov->base.hide    = overview_hide;
    ov->base.destroy = overview_destroy;

    overview_render(ov);
    return &ov->base;
}

static void overview_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)view;
    if (!ov) return;
    uint32_t now = now_ms();

    if (data) {
        switch (data->type) {
            case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
                const victron_record_battery_monitor_t *b = &data->record.battery;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = b->battery_current_milli;
                ov->bat.ttg_min = b->time_to_go_minutes;
                ov->bat.last_update_ms = now;
                energy_today_on_battery(b->battery_current_milli,
                                        b->battery_voltage_centi);
                ui_card_pulse(ov->card_bat);
                /* Si la batería descarga, también pulsa Loads (consumo activo) */
                if (b->battery_current_milli < -50) ui_card_pulse(ov->card_loads);
                break;
            }
            case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
                const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = (int32_t)b->battery_current_deci * 100;
                ov->bat.ttg_min = b->time_to_go_min;
                ov->bat.last_update_ms = now;
                ui_card_pulse(ov->card_bat);
                break;
            }
            case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
                const victron_record_solar_charger_t *s = &data->record.solar;
                ov->solar.has_data = true;
                ov->solar.pv_w = s->pv_power_w;
                ov->solar.load_current_deci = s->load_current_deci;
                ov->solar.voltage_centi = s->battery_voltage_centi;
                ov->solar.last_update_ms = now;
                energy_today_on_solar_yield(s->yield_today_centikwh);
                ui_card_pulse(ov->card_solar);
                /* Si SmartSolar reporta carga, también pulsa Loads */
                if (s->load_current_deci > 0) ui_card_pulse(ov->card_loads);
                /* Si no hay BMV, también usamos los datos de batería del Solar */
                if (!ov->bat.has_data) {
                    ov->bat.voltage_centi = s->battery_voltage_centi;
                    ov->bat.current_milli = (int32_t)s->battery_current_deci * 100;
                    ov->bat.last_update_ms = now;
                }
                break;
            }
            default: break;
        }
    }

    overview_render(ov);
}

static void overview_render(ui_overview_view_t *ov)
{
    if (!ov) return;
    uint32_t now = now_ms();
    const uint32_t TIMEOUT_MS = 30000;
    char buf[24];

    bool bat_fresh   = ov->bat.has_data &&
                       (now - ov->bat.last_update_ms) < TIMEOUT_MS;
    bool solar_fresh = ov->solar.has_data &&
                       (now - ov->solar.last_update_ms) < TIMEOUT_MS;

    /* ── Solar W ──────────────────────────────────────────────── */
    if (solar_fresh) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)ov->solar.pv_w);
        ui_metric_set(ov->m_solar_w, buf, "W",
                      ov->solar.pv_w > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_solar_w, "--", "W", UI_COLOR_TEXT_DIM);
    }

    /* ── Flecha Solar→Bat (W de carga desde PV) ────────────────── */
    unsigned solar_to_bat_w = solar_fresh ? ov->solar.pv_w : 0;
    if (solar_to_bat_w > 0) {
        snprintf(buf, sizeof(buf), "%uW", solar_to_bat_w);
        lv_label_set_text(ov->flow_solar, buf);
        lv_obj_set_style_text_color(ov->flow_solar, UI_COLOR_GREEN, 0);
        lv_obj_set_style_text_color(ov->arrow_solar, UI_COLOR_GREEN, 0);
    } else {
        lv_label_set_text(ov->flow_solar, "--");
        lv_obj_set_style_text_color(ov->flow_solar, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_color(ov->arrow_solar, UI_COLOR_TEXT_DIM, 0);
    }

    /* ── Batería: SOC + corriente ──────────────────────────────── */
    if (bat_fresh) {
        ui_arc_soc_set(ov->arc_soc, ov->bat.soc_deci, ov->bat.voltage_centi);
        int32_t mi = ov->bat.current_milli;
        int abs_a_centi = mi < 0 ? -mi/10 : mi/10;
        snprintf(buf, sizeof(buf), "%c%d.%02d",
                 mi >= 0 ? '+' : '-', abs_a_centi/100, abs_a_centi%100);
        ui_metric_set(ov->m_bat_current, buf, "A", ui_color_for_current(mi));
    } else {
        ui_arc_soc_set(ov->arc_soc, 0xFFFF, 0);
        ui_metric_set(ov->m_bat_current, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── Loads W (calculado) ──────────────────────────────────── */
    /* Prioridad 1: load_current del SmartSolar × V_bat
     * Prioridad 2: si bateria descarga, |I_bat| × V_bat */
    long loads_w = -1;
    if (solar_fresh && ov->solar.load_current_deci > 0 &&
        ov->solar.voltage_centi > 0) {
        loads_w = ((long)ov->solar.load_current_deci *
                   (long)ov->solar.voltage_centi) / 1000L;
    } else if (bat_fresh && ov->bat.current_milli < -50 &&
               ov->bat.voltage_centi > 0) {
        long abs_milli = -(long)ov->bat.current_milli;
        loads_w = (abs_milli * (long)ov->bat.voltage_centi) / 100000L;
    }
    if (loads_w >= 0) {
        snprintf(buf, sizeof(buf), "%ld", loads_w);
        ui_metric_set(ov->m_loads_w, buf, "W",
                      loads_w > 0 ? UI_COLOR_ORANGE : UI_COLOR_TEXT_DIM);
        snprintf(buf, sizeof(buf), "%ldW", loads_w);
        lv_label_set_text(ov->flow_loads, buf);
        lv_color_t flow_col = loads_w > 0 ? UI_COLOR_ORANGE : UI_COLOR_TEXT_DIM;
        lv_obj_set_style_text_color(ov->flow_loads, flow_col, 0);
        lv_obj_set_style_text_color(ov->arrow_loads, flow_col, 0);
    } else {
        ui_metric_set(ov->m_loads_w, "--", "W", UI_COLOR_TEXT_DIM);
        lv_label_set_text(ov->flow_loads, "--");
        lv_obj_set_style_text_color(ov->flow_loads, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_color(ov->arrow_loads, UI_COLOR_TEXT_DIM, 0);
    }

    /* ── TTG ──────────────────────────────────────────────────── */
    if (bat_fresh && ov->bat.ttg_min != 0xFFFFFFFF && ov->bat.ttg_min > 0) {
        if (ov->bat.ttg_min >= 60) {
            snprintf(buf, sizeof(buf), "%uh %02um",
                     (unsigned)(ov->bat.ttg_min / 60),
                     (unsigned)(ov->bat.ttg_min % 60));
        } else {
            snprintf(buf, sizeof(buf), "%um", (unsigned)ov->bat.ttg_min);
        }
        ui_metric_set(ov->m_ttg, buf, "", UI_COLOR_TEXT);
    } else {
        ui_metric_set(ov->m_ttg, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── Energía acumulada del día (PV + cargas) + comparativa ayer ── */
    if (ov->lbl_energy) {
        float pv  = energy_today_pv_kwh();
        float ld  = energy_today_loads_kwh();
        float ypv = energy_yesterday_pv_kwh();
        float yld = energy_yesterday_loads_kwh();
        bool any_today = (pv != 0.0f || ld != 0.0f || energy_today_is_fresh());
        bool any_yest  = (ypv != 0.0f || yld != 0.0f);
        if (!any_today && !any_yest) {
            lv_label_set_text(ov->lbl_energy, "PV --   Cargas --");
        } else if (!any_yest) {
            char ebuf[80];
            snprintf(ebuf, sizeof(ebuf),
                     "Hoy: PV %.2f kWh   Cargas %.2f kWh", pv, ld);
            lv_label_set_text(ov->lbl_energy, ebuf);
        } else {
            char ebuf[120];
            snprintf(ebuf, sizeof(ebuf),
                     "Hoy: PV %.2f  Cargas %.2f kWh   |   Ayer: PV %.2f  Cargas %.2f",
                     pv, ld, ypv, yld);
            lv_label_set_text(ov->lbl_energy, ebuf);
        }
    }
}

static void overview_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void overview_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void overview_destroy(ui_device_view_t *view)
{
    if (!view) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}
