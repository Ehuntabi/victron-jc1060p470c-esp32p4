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
static lv_obj_t *s_lbl_fan        = NULL;  /* legacy, no se usa (visual LED ahora) */
static lv_obj_t *s_fan_leds[2]    = { NULL, NULL };  /* 2 LEDs verdes: 50%, 100% */
static lv_obj_t *s_lbl_exterior_overlay = NULL;

static lv_obj_t *s_dd_aletas     = NULL;
static lv_obj_t *s_dd_congelador = NULL;
static lv_obj_t *s_dd_exterior   = NULL;

/* Flag para suprimir el callback de los dropdowns cuando refrescamos sus
 * selecciones programaticamente tras un swap. Sin esto entrariamos en
 * recursion: refresh -> set_selected -> VALUE_CHANGED -> callback -> swap... */
static bool s_suppress_dd_cb = false;

static lv_obj_t *s_lbl_tmin_val = NULL;
static lv_obj_t *s_lbl_tmax_val = NULL;

/* Segmented control "Modo": Auto / OFF / 50% / 100% (4 botones, el activo
 * se resalta azul). Y referencias a los 4 botones +/- de Min/Max para poder
 * deshabilitarlos cuando el modo no es AUTO. */
static lv_obj_t *s_btn_mode[4] = { NULL, NULL, NULL, NULL };
static lv_obj_t *s_btn_tmin_m  = NULL;
static lv_obj_t *s_btn_tmin_p  = NULL;
static lv_obj_t *s_btn_tmax_m  = NULL;
static lv_obj_t *s_btn_tmax_p  = NULL;

static ui_state_t *s_ui = NULL;

#define COL_NAME_W   250   /* ancho fijo columna nombre */
#define COL_VAL_W    200   /* ancho fijo columna valor  */
#define COL_DD_W     160   /* ancho fijo dropdown       */
#define COL_DD_PAD    20   /* margen derecho dropdown   */

/* ── Helpers modo ventilador (segmented control) ─────────────── */
/* Resalta visualmente el boton del modo activo y deja el resto en gris. */
static void highlight_mode_button(frigo_mode_t active)
{
    static const uint32_t ACTIVE_BG   = 0x4FC3F7; /* azul */
    static const uint32_t INACTIVE_BG = 0x444444; /* gris */
    for (int i = 0; i < 4; ++i) {
        if (!s_btn_mode[i]) continue;
        uint32_t bg = (i == (int)active) ? ACTIVE_BG : INACTIVE_BG;
        lv_obj_set_style_bg_color(s_btn_mode[i], lv_color_hex(bg), 0);
    }
}

/* En modo AUTO los thresholds Min/Max aplican: botones habilitados.
 * En modo manual (OFF/50/100) son irrelevantes: botones grises y bloqueados. */
static void thresholds_set_enabled(bool en)
{
    lv_obj_t *btns[4] = { s_btn_tmin_m, s_btn_tmin_p, s_btn_tmax_m, s_btn_tmax_p };
    for (int i = 0; i < 4; ++i) {
        if (!btns[i]) continue;
        if (en) {
            lv_obj_clear_state(btns[i], LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(btns[i], LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(btns[i], LV_OPA_30, 0);
        }
    }
}

static void apply_mode_visual(frigo_mode_t m)
{
    highlight_mode_button(m);
    thresholds_set_enabled(m == FRIGO_MODE_AUTO);
}

static void btn_mode_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    frigo_mode_t m = (frigo_mode_t)(intptr_t)lv_event_get_user_data(e);
    frigo_set_mode(m);
    apply_mode_visual(m);
}

/* ── Helpers asignacion con swap ─────────────────────────────── */
/* Refresca la seleccion visible de los 3 dropdowns segun el state actual,
 * SIN disparar sus VALUE_CHANGED (para no entrar en recursion via swap). */
static void refresh_dd_selections(void)
{
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
    s_suppress_dd_cb = true;
    if (s_dd_aletas)     lv_dropdown_set_selected(s_dd_aletas,     st->assignment[FRIGO_SLOT_ALETAS]);
    if (s_dd_congelador) lv_dropdown_set_selected(s_dd_congelador, st->assignment[FRIGO_SLOT_CONGELADOR]);
    if (s_dd_exterior)   lv_dropdown_set_selected(s_dd_exterior,   st->assignment[FRIGO_SLOT_EXTERIOR]);
    s_suppress_dd_cb = false;
}

/* Asigna new_idx al slot target. Si new_idx ya estaba asignado a OTRO slot,
 * hace swap: ese otro slot recibe lo que tenia el target. Garantiza
 * permutacion 1:1. */
static void apply_assignment_with_swap(frigo_slot_t target, uint8_t new_idx)
{
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
    uint8_t prev_in_target = st->assignment[target];
    for (int s = 0; s < 3; ++s) {
        if (s == (int)target) continue;
        if (st->assignment[s] == new_idx) {
            frigo_set_assignment((frigo_slot_t)s, prev_in_target);
            break;
        }
    }
    frigo_set_assignment(target, new_idx);
    refresh_dd_selections();
}

/* ── Callbacks ───────────────────────────────────────────────── */
static void dd_aletas_cb(lv_event_t *e)
{
    if (s_suppress_dd_cb) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    apply_assignment_with_swap(FRIGO_SLOT_ALETAS, (uint8_t)idx);
}
static void dd_congelador_cb(lv_event_t *e)
{
    if (s_suppress_dd_cb) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    apply_assignment_with_swap(FRIGO_SLOT_CONGELADOR, (uint8_t)idx);
}
static void dd_exterior_cb(lv_event_t *e)
{
    if (s_suppress_dd_cb) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
    apply_assignment_with_swap(FRIGO_SLOT_EXTERIOR, (uint8_t)idx);
}
static void btn_tmin_minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
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
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
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
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
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
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;
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
    /* Ancho para '-12.5 °C' (8 chars a 20pt ~ 100 px). 110 deja margen.
     * Texto justificado a la derecha: '-- °C' y '-12.5 °C' terminan en
     * el mismo borde junto al dropdown (no se desplazan a la izquierda). */
    lv_obj_set_width(lbl_val, 110);
    lv_obj_set_style_text_align(lbl_val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_val, "-- \xc2\xb0""C");
    *lbl_val_out = lbl_val;

    lv_obj_t *dd = lv_dropdown_create(sub);
    /* Ancho fijo razonable en vez de flex_grow(1) que lo expandia hasta
     * llenar toda la card. Cabe 'Sensor 8 (1A2B3C)' a 24pt. */
    lv_obj_set_width(dd, 220);
    lv_obj_set_height(dd, 50);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_24_es, 0);
    /* La flecha del dropdown (LV_PART_INDICATOR) es LV_SYMBOL_DOWN. Inter
     * (alias de _es) no incluye los LV_SYMBOL_* -> forzamos Montserrat
     * built-in para el indicator para que la flecha se vea. */
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_24, LV_PART_INDICATOR);
    lv_obj_t *dd_list = lv_dropdown_get_list(dd);
    if (dd_list) {
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_24_es, 0);
    }
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, dd_selected);
    lv_obj_add_event_cb(dd, dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *dd_out = dd;
    return row;
}

void ui_frigo_panel_init(ui_state_t *ui)
{
    s_ui = ui;
    frigo_state_t st_copy;
    frigo_get_state_copy(&st_copy);
    const frigo_state_t *st = &st_copy;

    lv_obj_t *tab = ui->frigo_page;
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_style_pad_gap(tab, 16, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    /* === Card 1: Sensores DS18B20 (azul) ===
     * Altura fija para que las dos cards (sensores y ventilador) tengan
     * exactamente el mismo tamano visual cuando van lado a lado. */
    lv_obj_t *card_sensors = lv_obj_create(tab);
    lv_obj_set_width(card_sensors, lv_pct(49));
    lv_obj_set_height(card_sensors, 380);
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

    /* Titulo. Montserrat built-in (no _es) porque Inter no tiene los
     * LV_SYMBOL_* y el LV_SYMBOL_LIST saldria invisible. */
    lv_obj_t *lbl_sec1 = lv_label_create(card_sensors);
    lv_obj_set_style_text_font(lbl_sec1, &lv_font_montserrat_24, 0);
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
    lv_obj_set_height(card_fan, 380);  /* misma altura que card_sensors */
    lv_obj_set_style_bg_color(card_fan, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_fan, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_fan, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card_fan, 1, 0);
    lv_obj_set_style_radius(card_fan, 12, 0);
    lv_obj_set_style_pad_all(card_fan, 16, 0);
    /* pad_gap mas amplio para que Auto/OFF y Min/Max esten claramente
     * separados visualmente (antes 12). */
    lv_obj_set_style_pad_gap(card_fan, 24, 0);
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
    /* Montserrat built-in para que el LV_SYMBOL_REFRESH renderice. */
    lv_obj_set_style_text_font(lbl_fan_sec, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_fan_sec, lv_color_hex(0x00C851), 0);
    lv_label_set_text(lbl_fan_sec, LV_SYMBOL_REFRESH "  Ventilador");
    /* Indicador LED del nivel del ventilador (estilo CLEAN_H de tanques):
     * 2 LEDs verdes que se encienden segun el porcentaje. Gris cuando off. */
    lv_obj_t *fan_ind = lv_obj_create(row_fan_hdr);
    lv_obj_remove_style_all(fan_ind);
    lv_obj_set_size(fan_ind, LV_SIZE_CONTENT, 20);
    lv_obj_set_layout(fan_ind, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(fan_ind, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fan_ind, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(fan_ind, 6, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *led = lv_obj_create(fan_ind);
        lv_obj_remove_style_all(led);
        lv_obj_set_size(led, 16, 16);
        lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(led, lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
        s_fan_leds[i] = led;
    }

    /* === Segmented control: Modo Auto / OFF / 50% / 100% === */
    lv_obj_t *row_mode = lv_obj_create(card_fan);
    lv_obj_remove_style_all(row_mode);
    lv_obj_set_width(row_mode, lv_pct(100));
    lv_obj_set_height(row_mode, LV_SIZE_CONTENT);
    lv_obj_set_layout(row_mode, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_mode, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mode, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_mode, 6, 0);
    static const char *mode_labels[4] = { "Auto", "OFF", "50%", "100%" };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *btn = lv_btn_create(row_mode);
        lv_obj_set_size(btn, 78, 44);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, mode_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20_es, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, btn_mode_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)i);
        s_btn_mode[i] = btn;
    }

    /* Separador visual entre el segmented control (Auto/OFF/50/100) y las
     * filas Min/Max para que queden claramente diferenciados. */
    lv_obj_t *sep = lv_obj_create(card_fan);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, lv_pct(85), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);

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

    s_btn_tmin_m = lv_btn_create(col_min);
    lv_obj_set_size(s_btn_tmin_m, 44, 44);
    lv_obj_set_style_radius(s_btn_tmin_m, 8, 0);
    lv_obj_set_style_bg_color(s_btn_tmin_m, lv_color_hex(0x444444), 0);
    lv_obj_t *lbl_mm = lv_label_create(s_btn_tmin_m);
    lv_label_set_text(lbl_mm, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_mm, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_mm);
    lv_obj_add_event_cb(s_btn_tmin_m, btn_tmin_minus_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_tmin_val = lv_label_create(col_min);
    lv_obj_set_style_text_font(s_lbl_tmin_val, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_tmin_val, lv_color_white(), 0);
    lv_obj_set_width(s_lbl_tmin_val, 80);
    lv_obj_set_style_text_align(s_lbl_tmin_val, LV_TEXT_ALIGN_CENTER, 0);
    { char buf[12]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", st->T_min); lv_label_set_text(s_lbl_tmin_val, buf); }

    s_btn_tmin_p = lv_btn_create(col_min);
    lv_obj_set_size(s_btn_tmin_p, 44, 44);
    lv_obj_set_style_radius(s_btn_tmin_p, 8, 0);
    lv_obj_set_style_bg_color(s_btn_tmin_p, lv_color_hex(0x4FC3F7), 0);
    lv_obj_t *lbl_mp = lv_label_create(s_btn_tmin_p);
    lv_label_set_text(lbl_mp, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_mp, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_mp);
    lv_obj_add_event_cb(s_btn_tmin_p, btn_tmin_plus_cb, LV_EVENT_CLICKED, NULL);

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

    s_btn_tmax_m = lv_btn_create(col_max);
    lv_obj_set_size(s_btn_tmax_m, 44, 44);
    lv_obj_set_style_radius(s_btn_tmax_m, 8, 0);
    lv_obj_set_style_bg_color(s_btn_tmax_m, lv_color_hex(0x444444), 0);
    lv_obj_t *lbl_xm = lv_label_create(s_btn_tmax_m);
    lv_label_set_text(lbl_xm, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_xm, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_xm);
    lv_obj_add_event_cb(s_btn_tmax_m, btn_tmax_minus_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_tmax_val = lv_label_create(col_max);
    lv_obj_set_style_text_font(s_lbl_tmax_val, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_tmax_val, lv_color_white(), 0);
    lv_obj_set_width(s_lbl_tmax_val, 80);
    lv_obj_set_style_text_align(s_lbl_tmax_val, LV_TEXT_ALIGN_CENTER, 0);
    { char buf[12]; snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", st->T_max); lv_label_set_text(s_lbl_tmax_val, buf); }

    s_btn_tmax_p = lv_btn_create(col_max);
    lv_obj_set_size(s_btn_tmax_p, 44, 44);
    lv_obj_set_style_radius(s_btn_tmax_p, 8, 0);
    lv_obj_set_style_bg_color(s_btn_tmax_p, lv_color_hex(0xFFAA00), 0);
    lv_obj_t *lbl_xp = lv_label_create(s_btn_tmax_p);
    lv_label_set_text(lbl_xp, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_xp, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_xp);
    lv_obj_add_event_cb(s_btn_tmax_p, btn_tmax_plus_cb, LV_EVENT_CLICKED, NULL);

    /* Estado visual inicial del segmented control + thresholds segun modo
     * actual (FRIGO_MODE_AUTO al boot por defecto, ver frigo_init). */
    apply_mode_visual(st->mode);

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
    /* LV_SIZE_CONTENT en ancho: el texto ya tiene longitud fija
     * (formato "%+6.1f" en update) asi que el cont mide siempre lo mismo
     * y la barra inferior no se reflowea. */
    lv_obj_set_size(overlay_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
/* alineacion gestionada por flex padre */

    static lv_font_t font_thermo_with_fallback;
    font_thermo_with_fallback = lv_font_thermometer;
    font_thermo_with_fallback.fallback = NULL;
    lv_obj_t *lbl_thermo_icon = lv_label_create(overlay_cont);
    lv_obj_set_style_text_font(lbl_thermo_icon, &font_thermo_with_fallback, 0);
    lv_obj_set_style_text_color(lbl_thermo_icon, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(lbl_thermo_icon, "\xef\x8b\x89");
    /* Texto fijo "Exterior:" pegado al icono del termometro (no cambia, asi
     * que no se desplaza). El numero va aparte, en su propia caja. */
    lv_obj_t *lbl_ext_prefix = lv_label_create(overlay_cont);
    lv_obj_add_style(lbl_ext_prefix, &ui->styles.small, 0);
    lv_obj_set_style_text_color(lbl_ext_prefix, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(lbl_ext_prefix, "Exterior:");

    /* Solo el valor: caja de ancho fijo alineada a la DERECHA. Asi el "\xc2\xb0""C"
     * queda clavado en el borde derecho y, al crecer el numero (o aparecer el
     * '-'), crece hacia la izquierda sin que los digitos se desplacen. El
     * ancho fijo evita ademas que la barra inferior se reflowee al cambiar el
     * valor. Ajustado a "%+5.1f" (caso peor "-55.0 \xc2\xb0""C", rango exterior)
     * para que el icono y "Exterior:" no queden lejos del valor. */
    s_lbl_exterior_overlay = lv_label_create(overlay_cont);
    lv_obj_add_style(s_lbl_exterior_overlay, &ui->styles.small, 0);
    lv_obj_set_style_text_color(s_lbl_exterior_overlay, lv_color_hex(0x00BFFF), 0);
    lv_obj_set_width(s_lbl_exterior_overlay, 124);
    lv_label_set_long_mode(s_lbl_exterior_overlay, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_lbl_exterior_overlay, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_lbl_exterior_overlay, "--.- \xc2\xb0""C");

    ESP_LOGI(TAG, "Panel frigo inicializado (%d sensores)", st->n_sensors);
}

/* ── Cerrar dropdowns abiertos ───────────────────────────────── */
void ui_frigo_panel_close_dropdowns(void)
{
    if (s_dd_aletas)     lv_dropdown_close(s_dd_aletas);
    if (s_dd_congelador) lv_dropdown_close(s_dd_congelador);
    if (s_dd_exterior)   lv_dropdown_close(s_dd_exterior);
}

/* ── Update ──────────────────────────────────────────────────── */
void ui_frigo_panel_update(ui_state_t *ui, const frigo_state_t *state)
{
    /* Reflejar cambios del modo del ventilador si vinieron desde otro lado
     * (ej. simulacion). En uso normal el cambio lo hace el callback de los
     * botones que ya repinta, asi que esto es defensivo. */
    static frigo_mode_t s_last_mode = (frigo_mode_t)0xFF;
    if (state->mode != s_last_mode) {
        apply_mode_visual(state->mode);
        s_last_mode = state->mode;
    }

    /* Si el numero de sensores detectados ha cambiado desde la ultima vez
     * (tipicamente: la UI se construyo antes de que frigo_init terminase la
     * enumeracion 1-Wire, asi que arrancaron los dropdowns vacios), regenerar
     * la lista de opciones y restaurar la asignacion guardada en NVS. */
    static uint8_t s_last_n = 0xFF;
    if (state->n_sensors != s_last_n) {
        char opts[128];
        build_sensor_options(opts, sizeof(opts), state);
        struct { lv_obj_t *dd; uint8_t slot; } dds[3] = {
            { s_dd_aletas,     FRIGO_SLOT_ALETAS     },
            { s_dd_congelador, FRIGO_SLOT_CONGELADOR },
            { s_dd_exterior,   FRIGO_SLOT_EXTERIOR   },
        };
        for (int i = 0; i < 3; ++i) {
            if (!dds[i].dd) continue;
            lv_dropdown_set_options(dds[i].dd, opts);
            uint8_t sel = state->assignment[dds[i].slot];
            if (state->n_sensors > 0 && sel >= state->n_sensors) sel = 0;
            lv_dropdown_set_selected(dds[i].dd, sel);
        }
        s_last_n = state->n_sensors;
    }

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
    /* Indicador LED del ventilador: 2 verdes.
     * Buckets: 0-24% -> 0 verdes, 25-74% -> 1 verde, 75-100% -> 2 verdes.
     * Matchea los modos discretos OFF/50%/100% y queda razonable en Auto. */
    {
        uint8_t pct = state->fan_percent;
        int leds_on = (pct >= 75) ? 2 : (pct >= 25) ? 1 : 0;
        lv_color_t on_color  = lv_color_hex(0x00C851);  /* verde */
        lv_color_t off_color = lv_color_hex(0x444444);  /* gris */
        for (int i = 0; i < 2; i++) {
            if (s_fan_leds[i]) {
                lv_obj_set_style_bg_color(s_fan_leds[i],
                                          (i < leds_on) ? on_color : off_color, 0);
            }
        }
    }
    if (s_lbl_exterior_overlay) {
        char buf[32];
        /* Solo el valor (el prefijo "Exterior:" es un label fijo aparte).
         * Ancho del numero fijo (%+5.1f -> 5 chars: " +5.4", "-12.5", "-55.0")
         * para que el label no cambie de tamano y la barra no se desplace. */
        if (state->T_Exterior < -120.0f)
            snprintf(buf, sizeof(buf), "--.- \xc2\xb0""C");
        else
            snprintf(buf, sizeof(buf), "%+5.1f \xc2\xb0""C", state->T_Exterior);
        lv_label_set_text(s_lbl_exterior_overlay, buf);
    }
}
