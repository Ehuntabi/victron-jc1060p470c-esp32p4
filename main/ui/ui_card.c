#include "ui_card.h"
#include "fonts/fonts_es.h"
#include <stdio.h>
#include <string.h>

/* ── Card contenedor ─────────────────────────────────────────────── */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_color_t border_color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, border_color, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, UI_PAD_CARD, 0);
    lv_obj_set_style_pad_gap(card, UI_GAP_CARD, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    /* main = vertical START (apila desde arriba), cross = HORIZONTAL CENTER
     * para que cualquier hijo SIZE_CONTENT quede centrado horizontalmente. */
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* Sombra exterior suave para sensación de elevación */
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(card, 0, 0);
    lv_obj_set_style_shadow_ofs_x(card, 0, 0);
    lv_obj_set_style_shadow_ofs_y(card, 4, 0);
    return card;
}

static void card_pulse_anim_cb(void *card, int32_t v)
{
    lv_obj_set_style_shadow_opa((lv_obj_t *)card, (lv_opa_t)v, 0);
}

void ui_card_pulse(lv_obj_t *card)
{
    if (!card) return;
    /* Cancela cualquier pulso anterior pendiente sobre esta card */
    lv_anim_del(card, card_pulse_anim_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_values(&a, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_time(&a, 250);
    lv_anim_set_playback_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, card_pulse_anim_cb);
    lv_anim_start(&a);
}

lv_obj_t *ui_card_set_title(lv_obj_t *card, const char *icon_utf8,
                            const char *title, lv_color_t accent)
{
    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 10, 0);
    /* Línea fina inferior con el color de acento como separador del body */
    lv_obj_set_style_pad_bottom(header, 8, 0);
    lv_obj_set_style_border_color(header, accent, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_30, 0);

    lv_obj_t *left = lv_obj_create(header);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left, 10, 0);

    if (icon_utf8 && icon_utf8[0]) {
        lv_obj_t *icon = lv_label_create(left);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28_es, 0);
        lv_obj_set_style_text_color(icon, accent, 0);
        lv_label_set_text(icon, icon_utf8);
    }

    lv_obj_t *lbl = lv_label_create(left);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_label_set_text(lbl, title ? title : "");

    return header;
}

lv_obj_t *ui_card_set_title_img(lv_obj_t *card, const lv_img_dsc_t *img_src,
                                const char *title, lv_color_t accent)
{
    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 12, 0);
    lv_obj_set_style_pad_bottom(header, 8, 0);
    lv_obj_set_style_border_color(header, accent, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_30, 0);

    lv_obj_t *left = lv_obj_create(header);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left, 12, 0);

    if (img_src) {
        lv_obj_t *img = lv_img_create(left);
        lv_img_set_src(img, img_src);
    }

    lv_obj_t *lbl = lv_label_create(left);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_label_set_text(lbl, title ? title : "");

    return header;
}

/* ── Métrica ─────────────────────────────────────────────────────── */
/* Estructura interna: contenedor flex column con
 *   child(0) = label "título"
 *   child(1) = fila valor+unidad
 *     child(1).child(0) = label "valor"
 *     child(1).child(1) = label "unidad"
 */
lv_obj_t *ui_metric_create(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    /* Centrar título y row valor+unidad horizontalmente */
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 2, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(title, label_text ? label_text : "");

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(row, 6, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(value, UI_COLOR_TEXT, 0);
    lv_label_set_text(value, "--");

    lv_obj_t *unit = lv_label_create(row);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(unit, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(unit, "");

    return box;
}

lv_obj_t *ui_metric_create_compact(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *box = ui_metric_create(parent, label_text);
    /* Para cards estrechas (~180-200 px ancho) con espacio vertical
     * abundante: title 24, value 46, unit 24 — aprovechando todo el alto. */
    lv_obj_t *title = lv_obj_get_child(box, 0);
    lv_obj_t *row   = lv_obj_get_child(box, 1);
    if (title) lv_obj_set_style_text_font(title, &lv_font_montserrat_24_es, 0);
    if (row) {
        lv_obj_t *value = lv_obj_get_child(row, 0);
        lv_obj_t *unit  = lv_obj_get_child(row, 1);
        if (value) lv_obj_set_style_text_font(value, &lv_font_montserrat_46, 0);
        if (unit)  lv_obj_set_style_text_font(unit,  &lv_font_montserrat_24_es, 0);
    }
    return box;
}

lv_obj_t *ui_metric_create_large(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *box = ui_metric_create(parent, label_text);
    /* Aumentar fuentes: title 24, value 46 (ASCII), unit 24 */
    lv_obj_t *title = lv_obj_get_child(box, 0);
    lv_obj_t *row   = lv_obj_get_child(box, 1);
    if (title) lv_obj_set_style_text_font(title, &lv_font_montserrat_24_es, 0);
    if (row) {
        lv_obj_t *value = lv_obj_get_child(row, 0);
        lv_obj_t *unit  = lv_obj_get_child(row, 1);
        if (value) lv_obj_set_style_text_font(value, &lv_font_montserrat_46, 0);
        if (unit)  lv_obj_set_style_text_font(unit,  &lv_font_montserrat_24_es, 0);
    }
    return box;
}

void ui_metric_set(lv_obj_t *metric, const char *value_str,
                   const char *unit_str, lv_color_t value_color)
{
    if (!metric) return;
    lv_obj_t *row = lv_obj_get_child(metric, 1);
    if (!row) return;
    lv_obj_t *value = lv_obj_get_child(row, 0);
    lv_obj_t *unit  = lv_obj_get_child(row, 1);
    if (value) {
        lv_label_set_text(value, value_str ? value_str : "--");
        /* Si el caller pasa color "negro/cero" mantenemos blanco como default */
        if (value_color.full == 0) value_color = UI_COLOR_TEXT;
        lv_obj_set_style_text_color(value, value_color, 0);
    }
    if (unit) lv_label_set_text(unit, unit_str ? unit_str : "");
}

void ui_metric_set_label(lv_obj_t *metric, const char *label_text,
                         lv_color_t label_color)
{
    if (!metric) return;
    lv_obj_t *title = lv_obj_get_child(metric, 0);
    if (!title) return;
    lv_label_set_text(title, label_text ? label_text : "");
    if (label_color.full == 0) label_color = UI_COLOR_TEXT_DIM;
    lv_obj_set_style_text_color(title, label_color, 0);
}

/* ── Pill ────────────────────────────────────────────────────────── */
lv_obj_t *ui_pill_create(lv_obj_t *parent, const char *text, lv_color_t bg)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, bg, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(pill, 16, 0);
    lv_obj_set_style_pad_ver(pill, 6, 0);
    /* Sombra interior leve para destacar */
    lv_obj_set_style_shadow_width(pill, 8, 0);
    lv_obj_set_style_shadow_color(pill, bg, 0);
    lv_obj_set_style_shadow_opa(pill, LV_OPA_30, 0);
    lv_obj_set_style_shadow_spread(pill, 0, 0);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_label_set_text(lbl, text ? text : "");
    return pill;
}

void ui_pill_set(lv_obj_t *pill, const char *text, lv_color_t bg)
{
    if (!pill) return;
    lv_obj_set_style_bg_color(pill, bg, 0);
    lv_obj_t *lbl = lv_obj_get_child(pill, 0);
    if (lbl) lv_label_set_text(lbl, text ? text : "");
}

/* ── Gauge SOC ───────────────────────────────────────────────────── */
/* Estructura: contenedor cuadrado con
 *   child(0) = arc (alineado al centro, fondo + indicador)
 *   child(1) = label central SOC% (font_28)
 *   child(2) = label sub voltaje (font_20)
 */
lv_obj_t *ui_arc_soc_create(lv_obj_t *parent, lv_coord_t size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);

    lv_obj_t *arc = lv_arc_create(box);
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, UI_COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_COLOR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_t *soc_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(soc_lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(soc_lbl, UI_COLOR_TEXT, 0);
    lv_label_set_text(soc_lbl, "--");
    lv_obj_align(soc_lbl, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *volt_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(volt_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(volt_lbl, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(volt_lbl, "--");
    lv_obj_align(volt_lbl, LV_ALIGN_CENTER, 0, 26);

    return box;
}

void ui_arc_soc_set(lv_obj_t *arc_box, uint16_t soc_deci, uint16_t voltage_centi)
{
    if (!arc_box) return;
    lv_obj_t *arc      = lv_obj_get_child(arc_box, 0);
    lv_obj_t *soc_lbl  = lv_obj_get_child(arc_box, 1);
    lv_obj_t *volt_lbl = lv_obj_get_child(arc_box, 2);

    char buf[16];
    if (soc_deci > 1000) {
        if (soc_lbl) lv_label_set_text(soc_lbl, "--");
        if (arc)     lv_arc_set_value(arc, 0);
    } else {
        unsigned int_part = soc_deci / 10;
        unsigned dec_part = soc_deci % 10;
        snprintf(buf, sizeof(buf), "%u.%u%%", int_part, dec_part);
        if (soc_lbl) lv_label_set_text(soc_lbl, buf);
        if (arc) {
            lv_arc_set_value(arc, soc_deci);
            lv_obj_set_style_arc_color(arc, ui_color_for_soc(soc_deci),
                                       LV_PART_INDICATOR);
        }
    }
    if (volt_lbl) {
        if (voltage_centi == 0) {
            lv_label_set_text(volt_lbl, "--");
        } else {
            snprintf(buf, sizeof(buf), "%u.%02u V",
                     voltage_centi / 100, voltage_centi % 100);
            lv_label_set_text(volt_lbl, buf);
        }
    }
}

/* ── Gauge SOC tipo "bateria de coche con bornes" ────────────────── */
/* Estructura del contenedor:
 *   child 0: terminals_row (fila con 2 bornes con simbolos + / -)
 *   child 1: body (carcasa oscura con 6 celdas separadas)
 *     child 0:        fill (rectangulo que sube de abajo)
 *     child 1..N-2:   separadores verticales decorativos (5)
 *     child ULTIMO:   soc_lbl (% en el centro)
 *   child 2: volt_lbl (voltaje debajo)
 *
 * ui_battery_soc_set toma fill = body.child(0) y soc_lbl = last child,
 * asi es robusto si anaden mas hijos decorativos a body. */
lv_obj_t *ui_battery_soc_create(lv_obj_t *parent,
                                lv_coord_t width, lv_coord_t height)
{
    const lv_coord_t term_h   = 14;
    const lv_coord_t term_w   = width / 4;
    const lv_coord_t volt_h   = 24;
    const lv_coord_t body_h   = height - term_h - volt_h - 4;

    /* Paleta realista: subida para contrastar con el card (0x141821) */
    const lv_color_t COL_CASING    = lv_color_hex(0x4a4a55); /* gris medio carcasa */
    const lv_color_t COL_CASING_HI = lv_color_hex(0x70707c); /* separadores celdas */
    const lv_color_t COL_BORDER    = lv_color_hex(0x2a2a30); /* borde oscuro casing */
    const lv_color_t COL_TOP_PLATE = lv_color_hex(0x2e2e36); /* franja superior */
    const lv_color_t COL_TERM_NEG  = lv_color_hex(0x9e9e9e); /* metalico gris claro */

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, width, height);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_gap(box, 0, 0);

    /* Fila de los dos bornes con simbolo + / - */
    lv_obj_t *terms = lv_obj_create(box);
    lv_obj_remove_style_all(terms);
    lv_obj_set_size(terms, width, term_h);
    lv_obj_set_layout(terms, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(terms, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(terms, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_left(terms,  width / 8, 0);
    lv_obj_set_style_pad_right(terms, width / 8, 0);

    /* Borne + (rojo) */
    lv_obj_t *plus = lv_obj_create(terms);
    lv_obj_remove_style_all(plus);
    lv_obj_set_size(plus, term_w, term_h);
    lv_obj_set_style_bg_color(plus, UI_COLOR_RED, 0);
    lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(plus, 3, 0);
    lv_obj_set_style_border_width(plus, 1, 0);
    lv_obj_set_style_border_color(plus, lv_color_hex(0x801010), 0);
    lv_obj_clear_flag(plus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lp = lv_label_create(plus);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_14_es, 0);
    lv_obj_set_style_text_color(lp, UI_COLOR_TEXT, 0);
    lv_label_set_text(lp, "+");
    lv_obj_center(lp);

    /* Borne - (gris metalico) */
    lv_obj_t *minus = lv_obj_create(terms);
    lv_obj_remove_style_all(minus);
    lv_obj_set_size(minus, term_w, term_h);
    lv_obj_set_style_bg_color(minus, COL_TERM_NEG, 0);
    lv_obj_set_style_bg_opa(minus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(minus, 3, 0);
    lv_obj_set_style_border_width(minus, 1, 0);
    lv_obj_set_style_border_color(minus, lv_color_hex(0x2a2a30), 0);
    lv_obj_clear_flag(minus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lm = lv_label_create(minus);
    lv_obj_set_style_text_font(lm, &lv_font_montserrat_14_es, 0);
    lv_obj_set_style_text_color(lm, UI_COLOR_TEXT, 0);
    lv_label_set_text(lm, "-");
    lv_obj_center(lm);

    /* Body — casing oscuro tipo polipropileno */
    lv_obj_t *body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, width, body_h);
    lv_obj_set_style_radius(body, 4, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, COL_BORDER, 0);
    lv_obj_set_style_bg_color(body, COL_CASING, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body, 4, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Fill (sube de abajo arriba con el SOC%) — CHILD 0 de body */
    lv_obj_t *fill = lv_obj_create(body);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, width - 12, 0);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(fill, 2, 0);
    lv_obj_set_style_bg_color(fill, UI_COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

    /* Franja oscura superior (simula la "tapa" donde van los tapones de celda) */
    lv_obj_t *top_plate = lv_obj_create(body);
    lv_obj_remove_style_all(top_plate);
    lv_obj_set_size(top_plate, width - 12, 7);
    lv_obj_align(top_plate, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_plate, COL_TOP_PLATE, 0);
    lv_obj_set_style_bg_opa(top_plate, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(top_plate, 1, 0);

    /* 5 separadores verticales para visualizar las 6 celdas internas */
    lv_coord_t inner_w = width - 8;
    for (int i = 1; i < 6; i++) {
        lv_obj_t *sep = lv_obj_create(body);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, 2, body_h - 8);
        lv_coord_t x = (inner_w * i / 6) - 1;
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, x, 0);
        lv_obj_set_style_bg_color(sep, COL_CASING_HI, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    }

    /* 6 tapones de celda en la franja superior (puntitos oscuros) */
    for (int i = 0; i < 6; i++) {
        lv_obj_t *cap = lv_obj_create(body);
        lv_obj_remove_style_all(cap);
        lv_obj_set_size(cap, 6, 4);
        lv_coord_t cx = (inner_w * (2*i + 1) / 12) - 3;
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, cx, 1);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0x1c1c20), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cap, 2, 0);
    }

    /* SOC% sobre el cuerpo — ULTIMO hijo de body (mas al frente) */
    lv_obj_t *soc_lbl = lv_label_create(body);
    lv_obj_set_style_text_font(soc_lbl, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(soc_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(soc_lbl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(soc_lbl, "--");
    lv_obj_align(soc_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Voltage debajo con separacion del cuerpo */
    lv_obj_t *volt_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(volt_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(volt_lbl, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_pad_top(volt_lbl, 8, 0);
    lv_label_set_text(volt_lbl, "--");

    return box;
}

void ui_battery_soc_set(lv_obj_t *bat_box,
                        uint16_t soc_deci, uint16_t voltage_centi)
{
    if (!bat_box) return;
    /* Estructura: bat_box.child(1)=body, bat_box.child(2)=volt_lbl.
     * body.child(0)=fill (siempre), body.last_child=soc_lbl.
     * En medio hay separadores y tapones decorativos. Usar last_child es
     * robusto a la cantidad de decoraciones. */
    lv_obj_t *body     = lv_obj_get_child(bat_box, 1);
    lv_obj_t *volt_lbl = lv_obj_get_child(bat_box, 2);
    if (!body || !volt_lbl) return;
    lv_obj_t *fill = lv_obj_get_child(body, 0);
    uint32_t nch = lv_obj_get_child_cnt(body);
    lv_obj_t *soc_lbl = (nch > 0) ? lv_obj_get_child(body, nch - 1) : NULL;
    if (!fill || !soc_lbl) return;

    /* Altura interna utilizable del body (sin paddings ni border) */
    lv_coord_t body_h = lv_obj_get_height(body);
    if (body_h <= 0) body_h = 100;
    lv_coord_t inner_h = body_h - 8;  /* 4 padding + 4 border approx */
    if (inner_h < 1) inner_h = 1;

    char buf[16];
    if (soc_deci > 1000) {
        lv_label_set_text(soc_lbl, "--");
        lv_obj_set_height(fill, 0);
    } else {
        unsigned int_part = soc_deci / 10;
        unsigned dec_part = soc_deci % 10;
        snprintf(buf, sizeof(buf), "%u.%u%%", int_part, dec_part);
        lv_label_set_text(soc_lbl, buf);
        lv_coord_t h = (lv_coord_t)((long)inner_h * (long)soc_deci / 1000L);
        if (h < 1) h = 1;
        lv_obj_set_height(fill, h);
        lv_obj_set_style_bg_color(fill, ui_color_for_soc(soc_deci), 0);
        lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (voltage_centi == 0) {
        lv_label_set_text(volt_lbl, "--");
    } else {
        snprintf(buf, sizeof(buf), "%u.%02u V",
                 voltage_centi / 100, voltage_centi % 100);
        lv_label_set_text(volt_lbl, buf);
    }
}

/* ── Tanque visual estilo "depósito" ─────────────────────────────── */
/* Estructura del contenedor:
 *   user_data: kind (cast a intptr_t para alarma)
 *   child 0: title_lbl (etiqueta arriba)
 *   child 1: tank (rectángulo del depósito, flex_grow 1 para llenar)
 *     child 0: fill (rectángulo que sube con el nivel)
 *     child 1: level_lbl (label grande con %)
 *
 * El tanque interno usa flex_grow 1 para resize automatico cuando el
 * caller cambia la altura del box (p.ej. via flex_grow del padre). */
lv_obj_t *ui_tank_create(lv_obj_t *parent, lv_coord_t width, lv_coord_t height,
                         const char *label_text, lv_color_t accent_color,
                         ui_tank_kind_t kind)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, width, height);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_gap(box, 4, 0);
    lv_obj_set_user_data(box, (void *)(intptr_t)kind);

    /* Etiqueta arriba (altura content), color claro accent para destacar */
    lv_obj_t *title = lv_label_create(box);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(title, accent_color, 0);
    lv_label_set_text(title, label_text ? label_text : "");

    /* Cuerpo del depósito: borde grueso brillante en color accent + fondo
     * oscuro azulado para que el relleno de agua resalte. */
    lv_obj_t *tank = lv_obj_create(box);
    lv_obj_remove_style_all(tank);
    lv_obj_set_width(tank, lv_pct(100));
    lv_obj_set_flex_grow(tank, 1);
    lv_obj_set_style_radius(tank, 10, 0);
    lv_obj_set_style_border_width(tank, 4, 0);
    lv_obj_set_style_border_color(tank, accent_color, 0);
    lv_obj_set_style_bg_color(tank, lv_color_hex(0x0a1620), 0);
    lv_obj_set_style_bg_opa(tank, LV_OPA_COVER, 0);
    /* Sombra interior sutil para dar sensacion de profundidad */
    lv_obj_set_style_shadow_width(tank, 8, 0);
    lv_obj_set_style_shadow_color(tank, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(tank, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(tank, -2, 0);
    lv_obj_set_style_pad_all(tank, 4, 0);
    lv_obj_clear_flag(tank, LV_OBJ_FLAG_SCROLLABLE);

    /* Fill (sube de abajo arriba con el nivel) */
    lv_obj_t *fill = lv_obj_create(tank);
    lv_obj_remove_style_all(fill);
    lv_obj_set_width(fill, lv_pct(100));
    lv_obj_set_height(fill, 0);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(fill, 6, 0);
    lv_obj_set_style_bg_color(fill, accent_color, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

    /* Label grande con el % - blanco brillante con sombra para legibilidad
     * sobre cualquier nivel de relleno (vacio o lleno). */
    lv_obj_t *lbl = lv_label_create(tank);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl, "--");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    return box;
}

void ui_tank_set(lv_obj_t *tank_box, uint8_t level_0_to_3)
{
    if (!tank_box) return;
    /* tank_box.child(0) = title, child(1) = tank.
     * tank.child(0) = fill, child(1) = level_lbl. */
    lv_obj_t *tank = lv_obj_get_child(tank_box, 1);
    if (!tank) return;
    lv_obj_t *fill = lv_obj_get_child(tank, 0);
    lv_obj_t *lbl  = lv_obj_get_child(tank, 1);
    if (!fill || !lbl) return;

    ui_tank_kind_t kind = (ui_tank_kind_t)(intptr_t)lv_obj_get_user_data(tank_box);

    /* Altura util interna (descontando padding+borde aprox) */
    lv_coord_t tank_h = lv_obj_get_content_height(tank);
    if (tank_h < 1) tank_h = lv_obj_get_height(tank) - 14;
    if (tank_h < 1) tank_h = 30;

    if (level_0_to_3 > 3) {
        lv_label_set_text(lbl, "--");
        lv_obj_set_height(fill, 0);
        return;
    }

    int pct = level_0_to_3 * 100 / 3;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", pct);
    lv_label_set_text(lbl, buf);

    lv_coord_t h = (lv_coord_t)((long)tank_h * (long)level_0_to_3 / 3L);
    if (h < 1) h = 1;
    lv_obj_set_height(fill, h);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Color por alarma */
    bool alarm = false;
    if (kind == UI_TANK_CLEAN && level_0_to_3 == 0) alarm = true;
    if (kind == UI_TANK_GREY  && level_0_to_3 == 3) alarm = true;
    lv_obj_set_style_bg_color(fill, alarm ? UI_COLOR_RED : UI_COLOR_CYAN, 0);
}

/* ── Helpers de color por rango ──────────────────────────────────── */
lv_color_t ui_color_for_soc(uint16_t soc_deci)
{
    if (soc_deci >= 700) return UI_COLOR_GREEN;
    if (soc_deci >= 300) return UI_COLOR_ORANGE;
    return UI_COLOR_RED;
}

lv_color_t ui_color_for_current(int32_t milli)
{
    /* Cualquier corriente positiva (entrando a batería) = verde (cargando).
     * Cualquier negativa (saliendo) = naranja (descargando).
     * Solo el cero exacto se queda gris. */
    if (milli > 0) return UI_COLOR_GREEN;
    if (milli < 0) return UI_COLOR_ORANGE;
    return UI_COLOR_TEXT_DIM;
}
