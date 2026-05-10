#include "fonts/fonts_es.h"
#include "lv_font_thermometer.h"
#include "ui/frigo_panel.h"
#include "alerts.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "FRIGO_PANEL";

/* ── Widgets ─────────────────────────────────────────────────── */
static lv_obj_t *s_lbl_aletas     = NULL;
static lv_obj_t *s_lbl_congelador = NULL;
static lv_obj_t *s_lbl_exterior   = NULL;
static lv_obj_t *s_lbl_fan        = NULL;
static lv_obj_t *s_lbl_exterior_overlay = NULL;

static lv_obj_t *s_dd_aletas     = NULL;
static lv_obj_t *s_dd_congelador = NULL;
static lv_obj_t *s_dd_exterior   = NULL;

static lv_obj_t *s_lbl_tmin_val = NULL;
static lv_obj_t *s_lbl_tmax_val = NULL;

static ui_state_t *s_ui = NULL;

#define COL_NAME_W   250   /* ancho fijo columna nombre */
#define COL_VAL_W    200   /* ancho fijo columna valor  */
#define COL_DD_W     160   /* ancho fijo dropdown       */
#define COL_DD_PAD    20   /* margen derecho dropdown   */

/* ── Callbacks ───────────────────────────────────────────────── */
static void dd_aletas_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    frigo_set_assignment(FRIGO_SLOT_ALETAS, (uint8_t)idx);
}
static void dd_congelador_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    frigo_set_assignment(FRIGO_SLOT_CONGELADOR, (uint8_t)idx);
}
static void dd_exterior_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    frigo_set_assignment(FRIGO_SLOT_EXTERIOR, (uint8_t)idx);
}
static void btn_tmin_minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const frigo_state_t *st = frigo_get_state();
    uint8_t t = st->T_min;
    if (t > 30) t -= 5;
    frigo_set_thresholds(t, st->T_max);
    if (s_lbl_tmin_val) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", t);
        lv_label_set_text(s_lbl_tmin_val, buf);
    }
}
static void btn_tmin_plus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const frigo_state_t *st = frigo_get_state();
    uint8_t t = st->T_min;
    if (t + 5 <= st->T_max) t += 5;
    frigo_set_thresholds(t, st->T_max);
    if (s_lbl_tmin_val) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", t);
        lv_label_set_text(s_lbl_tmin_val, buf);
    }
}
static void btn_tmax_minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const frigo_state_t *st = frigo_get_state();
    uint8_t t = st->T_max;
    if (t - 5 >= st->T_min) t -= 5;
    frigo_set_thresholds(st->T_min, t);
    if (s_lbl_tmax_val) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", t);
        lv_label_set_text(s_lbl_tmax_val, buf);
    }
}
static void btn_tmax_plus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const frigo_state_t *st = frigo_get_state();
    uint8_t t = st->T_max;
    if (t < 50) t += 5;
    frigo_set_thresholds(st->T_min, t);
    if (s_lbl_tmax_val) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", t);
        lv_label_set_text(s_lbl_tmax_val, buf);
    }
}

/* ── Construir opciones dropdown ─────────────────────────────── */
static void build_sensor_options(char *buf, size_t len, const frigo_state_t *st)
{
    buf[0] = '\0';
    for (int i = 0; i < st->n_sensors; i++) {
        char addr[28];
        frigo_addr_to_str(&st->sensors[i], addr, sizeof(addr));
        char line[36];
        snprintf(line, sizeof(line), "S%d: %s", i, addr + 18);
        if (i > 0) strncat(buf, "\n", len - strlen(buf) - 1);
        strncat(buf, line, len - strlen(buf) - 1);
    }
    if (st->n_sensors == 0) strncpy(buf, "Sin sensores", len);
}

/* ── Helper: crear fila de sensor ────────────────────────────── */
static lv_obj_t *make_sensor_row(lv_obj_t *parent, ui_state_t *ui,
                                  const char *nombre,
                                  lv_obj_t **lbl_val_out,
                                  lv_obj_t **dd_out,
                                  const char *opts,
                                  uint8_t dd_selected,
                                  lv_event_cb_t dd_cb)
{
    /* Contenedor en columna */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);

    /* Linea 1: Nombre */
    lv_obj_t *lbl_name = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_name, lv_color_white(), 0);
    lv_label_set_text(lbl_name, nombre);

    /* Linea 2: temperatura + dropdown */
    lv_obj_t *sub = lv_obj_create(row);
    lv_obj_remove_style_all(sub);
    lv_obj_set_width(sub, lv_pct(100));
    lv_obj_set_height(sub, LV_SIZE_CONTENT);
    lv_obj_set_layout(sub, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(sub, 8, 0);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_val = lv_label_create(sub);
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_width(lbl_val, 70);
    lv_label_set_text(lbl_val, "-- \xc2\xb0""C");
    *lbl_val_out = lbl_val;

    lv_obj_t *dd = lv_dropdown_create(sub);
    lv_obj_set_flex_grow(dd, 1);
    lv_obj_set_height(dd, 40);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, dd_selected);
    lv_obj_add_event_cb(dd, dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *dd_out = dd;
    return row;
}

void ui_frigo_panel_init(ui_state_t *ui)
{
    s_ui = ui;
    const frigo_state_t *st = frigo_get_state();

    lv_obj_t *tab = ui->frigo_page;
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_style_pad_gap(tab, 16, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    /* === Card 1: Sensores DS18B20 (azul) === */
    lv_obj_t *card_sensors = lv_obj_create(tab);
    lv_obj_set_width(card_sensors, lv_pct(49));
    lv_obj_set_height(card_sensors, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_sensors, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_sensors, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_sensors, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(card_sensors, 1, 0);
    lv_obj_set_style_radius(card_sensors, 12, 0);
    lv_obj_set_style_pad_all(card_sensors, 16, 0);
    lv_obj_set_style_pad_gap(card_sensors, 8, 0);
    lv_obj_set_layout(card_sensors, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_sensors, LV_FLEX_FLOW_COLUMN);

    char opts[128];
    build_sensor_options(opts, sizeof(opts), st);

    /* Titulo */
    lv_obj_t *lbl_sec1 = lv_label_create(card_sensors);
    lv_obj_set_style_text_font(lbl_sec1, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_sec1, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(lbl_sec1, LV_SYMBOL_LIST "  Sensores DS18B20");

    /* Filas sensores */
    make_sensor_row(card_sensors, ui, "Aletas:",
                    &s_lbl_aletas, &s_dd_aletas,
                    opts, st->assignment[FRIGO_SLOT_ALETAS], dd_aletas_cb);

    make_sensor_row(card_sensors, ui, "Congelador:",
                    &s_lbl_congelador, &s_dd_congelador,
                    opts, st->assignment[FRIGO_SLOT_CONGELADOR], dd_congelador_cb);

    make_sensor_row(card_sensors, ui, "Exterior:",
                    &s_lbl_exterior, &s_dd_exterior,
                    opts, st->assignment[FRIGO_SLOT_EXTERIOR], dd_exterior_cb);

    /* === Card 2: Ventilador y temperaturas (verde) === */
    lv_obj_t *card_fan = lv_obj_create(tab);
    lv_obj_set_width(card_fan, lv_pct(49));
    lv_obj_set_height(card_fan, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_fan, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_fan, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_fan, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card_fan, 1, 0);
    lv_obj_set_style_radius(card_fan, 12, 0);
    lv_obj_set_style_pad_all(card_fan, 16, 0);
    lv_obj_set_style_pad_gap(card_fan, 12, 0);
    lv_obj_set_layout(card_fan, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_fan, LV_FLEX_FLOW_COLUMN);


    /* Fila ventilador */
    lv_obj_t *row_fan_hdr = lv_obj_create(card_fan);
    lv_obj_remove_style_all(row_fan_hdr);
    lv_obj_set_style_bg_opa(row_fan_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_width(row_fan_hdr, lv_pct(100));
    lv_obj_set_height(row_fan_hdr, LV_SIZE_CONTENT);
    lv_obj_set_layout(row_fan_hdr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_fan_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_fan_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl_fan_sec = lv_label_create(row_fan_hdr);
    lv_obj_set_style_text_font(lbl_fan_sec, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_fan_sec, lv_color_hex(0x00C851), 0);
    lv_label_set_text(lbl_fan_sec, LV_SYMBOL_REFRESH "  Ventilador");
    s_lbl_fan = lv_label_create(row_fan_hdr);
    lv_obj_add_style(s_lbl_fan, &ui->styles.small, 0);
    lv_label_set_text(s_lbl_fan, "0 %");

    /* Fila T_Min y T_Max */
    lv_obj_t *row_t = lv_obj_create(card_fan);
    lv_obj_remove_style_all(row_t);
    lv_obj_set_style_bg_opa(row_t, LV_OPA_TRANSP, 0);
    lv_obj_set_width(row_t, lv_pct(100));
    lv_obj_set_height(row_t, LV_SIZE_CONTENT);
    lv_obj_set_layout(row_t, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row_t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_t, 12, 0);

    /* T_Min - layout horizontal compacto */
    lv_obj_t *col_min = lv_obj_create(row_t);
    lv_obj_remove_style_all(col_min);
    lv_obj_set_layout(col_min, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_min, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col_min, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_min, 8, 0);
    lv_obj_set_size(col_min, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *lbl_tmin = lv_label_create(col_min);
    lv_obj_set_style_text_font(lbl_tmin, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_tmin, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(lbl_tmin, "Min:");

    lv_obj_t *btn_min_m = lv_btn_create(col_min);
    lv_obj_set_size(btn_min_m, 44, 44);
    lv_obj_set_style_radius(btn_min_m, 8, 0);
    lv_obj_set_style_bg_color(btn_min_m, lv_color_hex(0x444444), 0);
    lv_obj_t *lbl_mm = lv_label_create(btn_min_m);
    lv_label_set_text(lbl_mm, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_mm, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_mm);
    lv_obj_add_event_cb(btn_min_m, btn_tmin_minus_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_tmin_val = lv_label_create(col_min);
    lv_obj_set_style_text_font(s_lbl_tmin_val, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_tmin_val, lv_color_white(), 0);
    lv_obj_set_width(s_lbl_tmin_val, 80);
    lv_obj_set_style_text_align(s_lbl_tmin_val, LV_TEXT_ALIGN_CENTER, 0);
    { char buf[12]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", st->T_min); lv_label_set_text(s_lbl_tmin_val, buf); }

    lv_obj_t *btn_min_p = lv_btn_create(col_min);
    lv_obj_set_size(btn_min_p, 44, 44);
    lv_obj_set_style_radius(btn_min_p, 8, 0);
    lv_obj_set_style_bg_color(btn_min_p, lv_color_hex(0x4FC3F7), 0);
    lv_obj_t *lbl_mp = lv_label_create(btn_min_p);
    lv_label_set_text(lbl_mp, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_mp, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_mp);
    lv_obj_add_event_cb(btn_min_p, btn_tmin_plus_cb, LV_EVENT_CLICKED, NULL);

    /* T_Max - layout horizontal compacto */
    lv_obj_t *col_max = lv_obj_create(row_t);
    lv_obj_remove_style_all(col_max);
    lv_obj_set_layout(col_max, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_max, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col_max, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_max, 8, 0);
    lv_obj_set_size(col_max, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *lbl_tmax = lv_label_create(col_max);
    lv_obj_set_style_text_font(lbl_tmax, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_tmax, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(lbl_tmax, "Max:");

    lv_obj_t *btn_max_m = lv_btn_create(col_max);
    lv_obj_set_size(btn_max_m, 44, 44);
    lv_obj_set_style_radius(btn_max_m, 8, 0);
    lv_obj_set_style_bg_color(btn_max_m, lv_color_hex(0x444444), 0);
    lv_obj_t *lbl_xm = lv_label_create(btn_max_m);
    lv_label_set_text(lbl_xm, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_xm, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_xm);
    lv_obj_add_event_cb(btn_max_m, btn_tmax_minus_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_tmax_val = lv_label_create(col_max);
    lv_obj_set_style_text_font(s_lbl_tmax_val, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_tmax_val, lv_color_white(), 0);
    lv_obj_set_width(s_lbl_tmax_val, 80);
    lv_obj_set_style_text_align(s_lbl_tmax_val, LV_TEXT_ALIGN_CENTER, 0);
    { char buf[12]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", st->T_max); lv_label_set_text(s_lbl_tmax_val, buf); }

    lv_obj_t *btn_max_p = lv_btn_create(col_max);
    lv_obj_set_size(btn_max_p, 44, 44);
    lv_obj_set_style_radius(btn_max_p, 8, 0);
    lv_obj_set_style_bg_color(btn_max_p, lv_color_hex(0xFFAA00), 0);
    lv_obj_t *lbl_xp = lv_label_create(btn_max_p);
    lv_label_set_text(lbl_xp, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_xp, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_xp);
    lv_obj_add_event_cb(btn_max_p, btn_tmax_plus_cb, LV_EVENT_CLICKED, NULL);

    /* Overlay Exterior */
    lv_obj_t *overlay_cont = lv_obj_create(ui->bottom_bar ? ui->bottom_bar : lv_scr_act());
    lv_obj_remove_style_all(overlay_cont);
    lv_obj_set_layout(overlay_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(overlay_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(overlay_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(overlay_cont, 4, 0);
    lv_obj_set_style_pad_all(overlay_cont, 4, 0);
    lv_obj_set_style_bg_opa(overlay_cont, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(overlay_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(overlay_cont, 4, 0);
    lv_obj_set_size(overlay_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
/* alineacion gestionada por flex padre */

    static lv_font_t font_thermo_with_fallback;
    font_thermo_with_fallback = lv_font_thermometer;
    font_thermo_with_fallback.fallback = NULL;
    lv_obj_t *lbl_thermo_icon = lv_label_create(overlay_cont);
    lv_obj_set_style_text_font(lbl_thermo_icon, &font_thermo_with_fallback, 0);
    lv_obj_set_style_text_color(lbl_thermo_icon, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(lbl_thermo_icon, "\xef\x8b\x89");
    s_lbl_exterior_overlay = lv_label_create(overlay_cont);
    lv_obj_add_style(s_lbl_exterior_overlay, &ui->styles.small, 0);
    lv_obj_set_style_text_color(s_lbl_exterior_overlay, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(s_lbl_exterior_overlay, "Exterior: -- \xc2\xb0""C");

    ESP_LOGI(TAG, "Panel frigo inicializado (%d sensores)", st->n_sensors);
}

/* ── Update ──────────────────────────────────────────────────── */
void ui_frigo_panel_update(ui_state_t *ui, const frigo_state_t *state)
{
    if (s_lbl_aletas) {
        char buf[16];
        if (state->T_Aletas < -120.0f)
            snprintf(buf, sizeof(buf), "-- \xc2\xb0""C");
        else
            snprintf(buf, sizeof(buf), "%.1f \xc2\xb0""C", state->T_Aletas);
        lv_label_set_text(s_lbl_aletas, buf);
    }
    if (s_lbl_congelador) {
        char buf[16];
        if (state->T_Congelador < -120.0f)
            snprintf(buf, sizeof(buf), "-- \xc2\xb0""C");
        else
            snprintf(buf, sizeof(buf), "%.1f \xc2\xb0""C", state->T_Congelador);
        lv_label_set_text(s_lbl_congelador, buf);
    }
    if (s_lbl_exterior) {
        char buf[16];
        if (state->T_Exterior < -120.0f)
            snprintf(buf, sizeof(buf), "-- \xc2\xb0""C");
        else
            snprintf(buf, sizeof(buf), "%.1f \xc2\xb0""C", state->T_Exterior);
        lv_label_set_text(s_lbl_exterior, buf);
    }
    if (s_lbl_fan) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d %%", state->fan_percent);
        lv_label_set_text(s_lbl_fan, buf);
    }
    if (s_lbl_exterior_overlay) {
        char buf[32];
        if (state->T_Exterior < -120.0f)
            snprintf(buf, sizeof(buf), "Exterior: -- \xc2\xb0""C");
        else
            snprintf(buf, sizeof(buf), "Exterior: %.1f \xc2\xb0""C", state->T_Exterior);
        lv_label_set_text(s_lbl_exterior_overlay, buf);
    }
}
