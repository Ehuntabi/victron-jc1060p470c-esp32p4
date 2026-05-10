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
