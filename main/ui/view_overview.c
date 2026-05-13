#include "view_overview.h"
#include "ui_card.h"
#include "energy_today.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "ui.h"
#include "ne185/ne185.h"
#include "frigo.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ui_device_view_t base;
    /* Cards */
    lv_obj_t *card_solar;
    lv_obj_t *m_solar_w;
    /* Card central Bateria */
    lv_obj_t *card_bat;
    lv_obj_t *arc_soc;
    lv_obj_t *m_bat_current;
    /* Card DC/DC (Orion-Tr Smart, carga desde batería motor) */
    lv_obj_t *card_loads;      /* nombre mantenido por compatibilidad */
    lv_obj_t *m_loads_w;       /* V_in: tensión batería motor */
    /* Métricas extra dentro de cada card */
    lv_obj_t *m_ttg;          /* dentro de card_bat: Autonomía */
    lv_obj_t *m_solar_kwh;    /* dentro de card_solar: kWh hoy */
    lv_obj_t *m_loads_kwh;    /* dentro de card_dcdc: V_out (servicio) */

    /* State-store DC/DC */
    struct {
        bool     has_data;
        uint8_t  state;       /* device_state: 0=off, otros valores=activos */
        int16_t  vin_centi;   /* tension entrada (bateria motor) */
        int16_t  vout_centi;  /* tension salida (bateria servicio) */
        uint32_t last_update_ms;
    } dcdc;

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

    /* ── Widgets camper (NE185 via UART) ─────────────────────── */
    lv_obj_t *tank_s1;           /* tanque agua limpia (debajo de Solar) */
    lv_obj_t *tank_r1;           /* tanque aguas grises (debajo de DC/DC) */
    lv_obj_t *pill_shore;        /* indicador 230 V (debajo de Bateria) */
    lv_obj_t *pill_shore_lbl;    /* texto interno del pill (ON/OFF) */
    /* ── Widgets frigo (DS18B20 + ventilador PWM) ─────────────── */
    lv_obj_t *lbl_freezer_temp;  /* T_Congelador (junto a tank limpia) */
    lv_obj_t *lbl_fan_pct;       /* % ventilador (junto a tank grises) */
    lv_obj_t *camper_bottom;     /* contenedor de los 3 botones */
    lv_obj_t *btn_lin;
    lv_obj_t *btn_lout;
    lv_obj_t *btn_pump;
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

/* ───────────────────────────────────────────────────────────────
 * Widgets camper (NE185): barra superior con tanques + 230V,
 * fila inferior con 3 botones (luz int, luz ext, bomba).
 * ─────────────────────────────────────────────────────────────── */
static void overview_camper_tick_cb(lv_timer_t *t)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov || !ov->base.root) return;
    if (lv_obj_has_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN)) return;
    overview_render(ov);
}

static void camper_btn_event_cb(lv_event_t *e)
{
    char cmd = (char)(intptr_t)lv_event_get_user_data(e);
    ne185_send_cmd(cmd);
}

/* (helper camper_make_tank antiguo eliminado: ahora se usa
 *  ui_tank_create de ui_card.c, que es el widget visual grande) */

static lv_obj_t *camper_make_button(lv_obj_t *parent, const char *text,
                                    char cmd_char)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 140, 70);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, camper_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)cmd_char);
    return btn;
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

    /* ── Grid principal: 3 columnas verticales ──────────────── */
    lv_obj_t *grid = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Columna izquierda: card Solar arriba + tanque limpia abajo ── */
    lv_obj_t *col_left = lv_obj_create(grid);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_height(col_left, lv_pct(100));
    lv_obj_set_flex_grow(col_left, 3);
    lv_obj_set_layout(col_left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_left, 8, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_solar = create_node_card(col_left, &icon_solar,
                                      "Solar", UI_COLOR_GREEN, &ov->m_solar_w);
    lv_obj_set_width(ov->card_solar, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_solar, 8);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_solar_w, "Actual", UI_COLOR_TEXT_DIM);
    ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT);
    ov->m_solar_kwh = ui_metric_create_compact(ov->card_solar, "Hoy");
    ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);

    ov->tank_s1 = ui_tank_create(col_left, lv_pct(75), 1,
                                 "Agua limpia", UI_COLOR_CYAN, UI_TANK_CLEAN);
    lv_obj_set_flex_grow(ov->tank_s1, 3);

    /* ── Columna central: card Bateria arriba + indicador 230V abajo ── */
    lv_obj_t *col_center = lv_obj_create(grid);
    lv_obj_remove_style_all(col_center);
    lv_obj_set_height(col_center, lv_pct(100));
    lv_obj_set_flex_grow(col_center, 5);
    lv_obj_set_layout(col_center, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_center, 8, 0);
    lv_obj_clear_flag(col_center, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_bat = ui_card_create(col_center, UI_COLOR_ORANGE);
    lv_obj_set_width(ov->card_bat, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_bat, 6);
    ui_card_set_title_img(ov->card_bat, &icon_battery,
                          "Batería", UI_COLOR_ORANGE);
    ov->arc_soc = ui_battery_soc_create(ov->card_bat, 120, 112);
    /* Fila horizontal con Corriente + Autonomía */
    lv_obj_t *bat_row = lv_obj_create(ov->card_bat);
    lv_obj_remove_style_all(bat_row);
    lv_obj_set_width(bat_row, lv_pct(100));
    lv_obj_set_height(bat_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(bat_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bat_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_row, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bat_row, LV_OBJ_FLAG_SCROLLABLE);
    ov->m_bat_current = ui_metric_create_compact(bat_row, "Corriente");
    ov->m_ttg         = ui_metric_create_compact(bat_row, "Autonomía");

    /* Bottom de col_center: fila horizontal con [Congelador, 230V, Ventilador] */
    {
        lv_obj_t *bottom_row = lv_obj_create(col_center);
        lv_obj_remove_style_all(bottom_row);
        lv_obj_set_width(bottom_row, lv_pct(100));
        lv_obj_set_flex_grow(bottom_row, 1);
        lv_obj_set_layout(bottom_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bottom_row, LV_FLEX_ALIGN_SPACE_AROUND,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(bottom_row, 6, 0);
        lv_obj_clear_flag(bottom_row, LV_OBJ_FLAG_SCROLLABLE);

        /* Congelador (izquierda) */
        lv_obj_t *col_freezer = lv_obj_create(bottom_row);
        lv_obj_remove_style_all(col_freezer);
        lv_obj_set_size(col_freezer, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(col_freezer, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(col_freezer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col_freezer, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(col_freezer, 2, 0);
        lv_obj_t *t_lbl = lv_label_create(col_freezer);
        lv_obj_set_style_text_font(t_lbl, &lv_font_montserrat_20_es, 0);
        lv_obj_set_style_text_color(t_lbl, UI_COLOR_CYAN, 0);
        lv_label_set_text(t_lbl, "Congelador");
        ov->lbl_freezer_temp = lv_label_create(col_freezer);
        lv_obj_set_style_text_font(ov->lbl_freezer_temp, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(ov->lbl_freezer_temp, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->lbl_freezer_temp, "-- \xc2\xb0""C");

        /* Pill 230 V (centro) */
        lv_obj_t *pill = lv_obj_create(bottom_row);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(pill, 14, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_border_color(pill, UI_COLOR_CARD_BORDER, 0);
        lv_obj_set_style_bg_color(pill, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 16, 0);
        lv_obj_set_style_pad_ver(pill, 8, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        ov->pill_shore_lbl = lv_label_create(pill);
        lv_obj_set_style_text_font(ov->pill_shore_lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(ov->pill_shore_lbl, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->pill_shore_lbl, "230 V");
        lv_obj_center(ov->pill_shore_lbl);
        ov->pill_shore = pill;

        /* Ventilador (derecha) */
        lv_obj_t *col_fan = lv_obj_create(bottom_row);
        lv_obj_remove_style_all(col_fan);
        lv_obj_set_size(col_fan, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(col_fan, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(col_fan, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col_fan, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(col_fan, 2, 0);
        lv_obj_t *f_lbl = lv_label_create(col_fan);
        lv_obj_set_style_text_font(f_lbl, &lv_font_montserrat_20_es, 0);
        lv_obj_set_style_text_color(f_lbl, UI_COLOR_CYAN, 0);
        lv_label_set_text(f_lbl, "Ventilador");
        ov->lbl_fan_pct = lv_label_create(col_fan);
        lv_obj_set_style_text_font(ov->lbl_fan_pct, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(ov->lbl_fan_pct, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->lbl_fan_pct, "-- %");
    }

    /* ── Columna derecha: card DC/DC arriba + tanque grises abajo ── */
    lv_obj_t *col_right = lv_obj_create(grid);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_height(col_right, lv_pct(100));
    lv_obj_set_flex_grow(col_right, 3);
    lv_obj_set_layout(col_right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_right, 8, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_loads = create_node_card(col_right, &icon_dcdc,
                                      "DC/DC", UI_COLOR_CYAN, &ov->m_loads_w);
    lv_obj_set_width(ov->card_loads, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_loads, 8);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_loads_w, "Motor", UI_COLOR_TEXT_DIM);
    ui_metric_set(ov->m_loads_w, "--", "V", UI_COLOR_TEXT);
    ov->m_loads_kwh = ui_metric_create_compact(ov->card_loads, "Servicio");
    ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);

    ov->tank_r1 = ui_tank_create(col_right, lv_pct(75), 1,
                                 "Aguas grises", UI_COLOR_CYAN, UI_TANK_GREY);
    lv_obj_set_flex_grow(ov->tank_r1, 3);

    /* ── Camper bottom: 3 botones grandes ────────────────────── */
    ov->camper_bottom = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(ov->camper_bottom);
    lv_obj_set_width(ov->camper_bottom, lv_pct(100));
    lv_obj_set_height(ov->camper_bottom, LV_SIZE_CONTENT);
    lv_obj_set_layout(ov->camper_bottom, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ov->camper_bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ov->camper_bottom, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ov->camper_bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(ov->camper_bottom, 6, 0);

    ov->btn_lin  = camper_make_button(ov->camper_bottom, "Luz INT",  'i');
    ov->btn_lout = camper_make_button(ov->camper_bottom, "Luz EXT",  'o');
    ov->btn_pump = camper_make_button(ov->camper_bottom, "Bomba",    'p');

    /* Timer LVGL para refrescar los widgets camper aunque no llegue
     * dato Victron. Cada 500 ms re-renderiza la vista. */
    lv_timer_create(overview_camper_tick_cb, 500, ov);

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
                break;
            }
            case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
                const victron_record_dcdc_converter_t *c = &data->record.dcdc;
                ov->dcdc.has_data = true;
                ov->dcdc.state      = c->device_state;
                ov->dcdc.vin_centi  = (int16_t)c->input_voltage_centi;
                ov->dcdc.vout_centi = (int16_t)c->output_voltage_centi;
                ov->dcdc.last_update_ms = now;
                ui_card_pulse(ov->card_loads);
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

    /* ── Solar A (corriente de carga PV→batería) ──────────────── */
    /* En SmartSolar BLE, battery_current_deci está en décimas de A
     * sobre el solar.pv_w (potencia). Si no tenemos battery_current_deci
     * publicado en el state-store, derivamos A = W/V. */
    long solar_centi_a = -1;  /* en cA (centi-amperios) */
    if (solar_fresh && ov->solar.voltage_centi > 0) {
        /* I = P/V → centi-A = W * 10000 / voltage_centi */
        solar_centi_a = ((long)ov->solar.pv_w * 10000L) /
                        (long)ov->solar.voltage_centi;
    }
    if (solar_centi_a >= 0) {
        snprintf(buf, sizeof(buf), "%ld.%02ld",
                 solar_centi_a / 100, solar_centi_a % 100);
        ui_metric_set(ov->m_solar_w, buf, "A",
                      solar_centi_a > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT_DIM);
    }

    /* (Las flechas Solar→Bat / Bat→DC se eliminaron al pasar a layout
     * de 3 columnas verticales. El flujo se infiere de los colores.) */

    /* ── Batería: SOC + corriente ──────────────────────────────── */
    if (bat_fresh) {
        ui_battery_soc_set(ov->arc_soc, ov->bat.soc_deci, ov->bat.voltage_centi);
        int32_t mi = ov->bat.current_milli;
        int abs_a_centi = mi < 0 ? -mi/10 : mi/10;
        snprintf(buf, sizeof(buf), "%c%d.%02d",
                 mi >= 0 ? '+' : '-', abs_a_centi/100, abs_a_centi%100);
        ui_metric_set(ov->m_bat_current, buf, "A", ui_color_for_current(mi));
    } else {
        ui_battery_soc_set(ov->arc_soc, 0xFFFF, 0);
        ui_metric_set(ov->m_bat_current, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── DC/DC (Orion-Tr Smart): muestra V_in (motor) y V_out (servicio) ── */
    bool dcdc_fresh = ov->dcdc.has_data &&
                      (now - ov->dcdc.last_update_ms) < TIMEOUT_MS;
    if (dcdc_fresh) {
        /* V_in (batería motor) — métrica principal grande */
        snprintf(buf, sizeof(buf), "%u.%02u",
                 ov->dcdc.vin_centi / 100, ov->dcdc.vin_centi % 100);
        bool active = ov->dcdc.state != 0;
        ui_metric_set(ov->m_loads_w, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
        /* V_out (batería servicio) — métrica pequeña */
        snprintf(buf, sizeof(buf), "%u.%02u",
                 ov->dcdc.vout_centi / 100, ov->dcdc.vout_centi % 100);
        ui_metric_set(ov->m_loads_kwh, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_loads_w,   "--", "V", UI_COLOR_TEXT_DIM);
        ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);
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

    /* ── Camper (NE185 vía UART) ─────────────────────────────── */
    {
        ne185_data_t cd;
        ne185_get(&cd);

        /* Tanques visuales: nivel 0..3 (ui_tank_set ignora valores > 3) */
        if (ov->tank_s1) ui_tank_set(ov->tank_s1, cd.fresh ? cd.s1 : 0xFF);
        if (ov->tank_r1) ui_tank_set(ov->tank_r1, cd.fresh ? cd.r1 : 0xFF);

        /* Indicador 230 V grande */
        if (ov->pill_shore) {
            lv_obj_set_style_bg_color(ov->pill_shore,
                cd.fresh && cd.shore ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM, 0);
        }
        /* El texto "230 V" se mantiene fijo; solo cambia el color del pill */
        /* Botones: color verde si la salida está activa */
        if (ov->btn_lin) {
            lv_obj_set_style_bg_color(ov->btn_lin,
                cd.fresh && cd.light_in ? UI_COLOR_YELLOW : UI_COLOR_TEXT_DIM, 0);
        }
        if (ov->btn_lout) {
            lv_obj_set_style_bg_color(ov->btn_lout,
                cd.fresh && cd.light_out ? UI_COLOR_YELLOW : UI_COLOR_TEXT_DIM, 0);
        }
        if (ov->btn_pump) {
            lv_obj_set_style_bg_color(ov->btn_pump,
                cd.fresh && cd.pump ? UI_COLOR_CYAN : UI_COLOR_TEXT_DIM, 0);
        }
    }

    /* ── Frigo: T_Congelador y % ventilador ─────────────────── */
    {
        const frigo_state_t *fs = frigo_get_state();
        if (fs && ov->lbl_freezer_temp) {
            char fbuf[16];
            if (fs->T_Congelador > -120.0f) {
                snprintf(fbuf, sizeof(fbuf), "%.1f \xc2\xb0""C",
                         fs->T_Congelador);
                lv_obj_set_style_text_color(ov->lbl_freezer_temp,
                    fs->T_Congelador > -2.0f ? UI_COLOR_RED
                                             : UI_COLOR_TEXT, 0);
            } else {
                snprintf(fbuf, sizeof(fbuf), "-- \xc2\xb0""C");
                lv_obj_set_style_text_color(ov->lbl_freezer_temp,
                                            UI_COLOR_TEXT_DIM, 0);
            }
            lv_label_set_text(ov->lbl_freezer_temp, fbuf);
        }
        if (fs && ov->lbl_fan_pct) {
            char fbuf[8];
            snprintf(fbuf, sizeof(fbuf), "%u %%", (unsigned)fs->fan_percent);
            lv_label_set_text(ov->lbl_fan_pct, fbuf);
            lv_obj_set_style_text_color(ov->lbl_fan_pct,
                fs->fan_percent > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM, 0);
        }
    }

    /* ── Energía solar acumulada del día (kWh hoy en card Solar) ── */
    {
        float pv = energy_today_pv_kwh();
        bool fresh_today = energy_today_is_fresh();
        char ebuf[16];
        if (ov->m_solar_kwh) {
            if (pv > 0.0f || fresh_today) {
                snprintf(ebuf, sizeof(ebuf), "%.2f", pv);
                ui_metric_set(ov->m_solar_kwh, ebuf, "kWh",
                              pv > 0.0f ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
            } else {
                ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);
            }
        }
        /* card_dcdc: m_loads_kwh ya lo gestiona la rama DC/DC arriba con
         * V_out (servicio), no kWh. No tocar aquí. */
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
