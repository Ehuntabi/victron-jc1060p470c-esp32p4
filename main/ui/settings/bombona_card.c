/* Tarjeta "Bombonas" de Ajustes -> Autocaravana: un boton para apuntar el
 * cambio y la estadistica de cuanto duran.
 *
 * Por que existe (peticion del usuario, 24-ago-2026): COMPRAR una bombona ya lo
 * apunta el cuaderno de la cabina como gasto, pero comprarla y usarla son dos
 * momentos distintos -- puede pasarse meses de repuesto en el hueco. Lo que
 * mide cuanto dura es el CAMBIO, y eso no se apuntaba en ningun sitio.
 *
 * Va en la P4 y no en la 3,5" a proposito: la bombona se cambia con el vehiculo
 * parado y por fuera, que es justo donde esta esta pantalla. Lo que se decide
 * conduciendo va en la de la cabina (ver la nota del proyecto sobre eso).
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "fonts/fonts_es.h"
#include "data/bombonas.h"

#include <stdio.h>
#include <time.h>
#include <lvgl.h>

static lv_obj_t *s_lbl;

void bombona_label_refresh(void)
{
    if (!s_lbl || !lv_obj_is_visible(s_lbl)) return;

    bombonas_t b;
    bombonas_get(&b);
    char buf[288];

    if (b.n == 0) {
        snprintf(buf, sizeof(buf),
                 "Todavia no hay ningun cambio apuntado.\n"
                 "Pulsa el boton al poner una bombona nueva.");
        lv_label_set_text(s_lbl, buf);
        return;
    }

    char puesta[24];
    time_t t = (time_t)b.cambios[b.n - 1];
    struct tm tm_l;
    localtime_r(&t, &tm_l);
    strftime(puesta, sizeof(puesta), "%d/%m/%Y %H:%M", &tm_l);

    int dias = bombonas_dias_actual();
    float media = bombonas_media_dias();

    /* Segunda linea: las ultimas duraciones, de la mas reciente hacia atras.
     * Ver los numeros sueltos dice mas que la media -- una bombona de invierno
     * y otra de verano no duran igual, y la media sola lo esconde. */
    char ultimas[120] = "";
    size_t u = 0;
    for (int i = b.n - 1; i >= 1 && u < sizeof(ultimas) - 12; i--) {
        if (b.cambios[i] <= b.cambios[i - 1]) continue;
        int d = (int)((b.cambios[i] - b.cambios[i - 1]) / 86400);
        int w = snprintf(ultimas + u, sizeof(ultimas) - u, "%s%d", u ? ", " : "", d);
        if (w < 0) break;
        u += (size_t)w;
    }

    int n = snprintf(buf, sizeof(buf), "Puesta el %s", puesta);
    if (dias >= 0) n += snprintf(buf + n, sizeof(buf) - n, "   |   lleva %d dia%s",
                                 dias, dias == 1 ? "" : "s");
    if (media > 0.0f) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "\nDuran %.0f dias de media", (double)media);
        if (u) snprintf(buf + n, sizeof(buf) - n, "   (ultimas: %s)", ultimas);
    } else {
        snprintf(buf + n, sizeof(buf) - n,
                 "\nHace falta un cambio mas para saber cuanto dura una.");
    }
    lv_label_set_text(s_lbl, buf);
}

static void do_cambio(void)
{
    if (bombonas_cambio()) {
        bombona_label_refresh();
    } else {
        /* El unico motivo de fallo es el reloj sin poner en hora. Se dice, en
         * vez de apuntar una fecha inventada que luego daria una duracion
         * falsa y creible. */
        ui_show_info_dialog(LV_SYMBOL_WARNING "  No he podido apuntarlo",
            "La pantalla todavia no tiene la hora en hora.\n\n"
            "Espera a que se ponga (con el GPS o la red)\n"
            "y vuelve a pulsar.");
    }
}

static void cambio_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_REFRESH "  Cambio de bombona",
        "Apuntar que acabas de poner una bombona nueva?\n\n"
        "Es lo que mide cuanto dura cada una, asi que\n"
        "solo al CAMBIARLA, no al comprarla.",
        "Apuntar", do_cambio);
}

static void do_deshacer(void)
{
    bombonas_deshacer();
    bombona_label_refresh();
}

static void deshacer_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_WARNING "  Deshacer",
        "Borrar el ultimo cambio apuntado?\n\n"
        "Para cuando se pulsa sin querer: si no,\n"
        "un toque estropea la estadistica para siempre.",
        "Borrar", do_deshacer);
}

void create_bombona_card(lv_obj_t *cont)
{
    lv_obj_t *card = lv_obj_create(cont);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFFA726), 0);   /* naranja: gas */
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_gap(card, 4, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(head, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFA726), 0);
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  Bombonas");

    lv_obj_t *btns = lv_obj_create(head);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);

    lv_obj_t *b1 = lv_btn_create(btns);
    lv_obj_set_size(b1, 220, 44);
    lv_obj_set_style_bg_color(b1, lv_color_hex(0xF57C00), 0);
    lv_obj_set_style_radius(b1, 8, 0);
    lv_obj_t *l1 = lv_label_create(b1);
    lv_label_set_text(l1, "Cambio de bombona");
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_20_es, 0);
    lv_obj_center(l1);
    lv_obj_add_event_cb(b1, cambio_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_btn_create(btns);
    lv_obj_set_size(b2, 120, 44);
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_radius(b2, 8, 0);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, "Deshacer");
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_20_es, 0);
    lv_obj_center(l2);
    lv_obj_add_event_cb(b2, deshacer_cb, LV_EVENT_CLICKED, NULL);

    s_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(s_lbl, lv_pct(100));
    /* LONG_DOT y no WRAP, igual que el resto de tarjetas de Ajustes: el wrap al
     * construir ha dado sustos con el watchdog. */
    lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_DOT);
    bombona_label_refresh();
}
