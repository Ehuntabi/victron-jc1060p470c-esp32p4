/* Dialogo modal generico (confirmacion / aviso) que usan varias paginas de
 * Ajustes: Trip computer, Reiniciar, etc.
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver
 * settings_common.h). Es el mismo codigo movido de sitio, sin cambios de
 * comportamiento.
 */
#include "settings_common.h"
#include "fonts/fonts_es.h"

#include <string.h>
#include <lvgl.h>

/* === Dialogo de confirmacion con el estilo del aviso de Victron Keys =======
 * Modal grande (600x280), fondo oscurecido, borde y titulo rosa, texto grande
 * centrado y botones 220x60. Un unico helper para TODAS las confirmaciones, asi
 * quedan identicas en estilo y tamano. La accion (on_confirm) se ejecuta solo si
 * se pulsa el boton derecho; el modal se cierra siempre. */
static lv_obj_t *s_confirm_modal = NULL;
static ui_confirm_action_t s_confirm_action = NULL;

static void ui_confirm_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    bool ok = (txt && strcmp(txt, "Cancelar") != 0);  /* el izquierdo siempre es Cancelar */
    ui_confirm_action_t action = s_confirm_action;
    if (s_confirm_modal) { lv_obj_del(s_confirm_modal); s_confirm_modal = NULL; }
    s_confirm_action = NULL;
    if (ok && action) action();
}

void ui_show_confirm_dialog(const char *title, const char *msg,
                            const char *ok_txt, ui_confirm_action_t on_confirm)
{
    if (s_confirm_modal) return;
    s_confirm_action = on_confirm;

    /* Fondo modal a pantalla completa (mismo que el aviso de Victron Keys) */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_confirm_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_lbl = lv_label_create(dlg);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title_lbl, title);

    lv_obj_t *msg_lbl = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl, lv_pct(100));
    lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg_lbl, msg);

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 220, 60);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, ui_confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 220, 60);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, ok_txt);
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, ui_confirm_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* Aviso de solo lectura: el mismo modal pero con un unico boton y sin accion.
 * Se usa al finalizar viaje para decir que ya se puede sacar la tarjeta. */
void ui_show_info_dialog(const char *title, const char *msg)
{
    ui_show_confirm_dialog(title, msg, "Entendido", NULL);
}
