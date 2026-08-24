/* Energia del viaje: tarjeta de Ajustes (Autocaravana) con la energia del viaje.
 *
 * OJO, cambio de fondo el 24-ago-2026: esta tarjeta YA NO abre ni cierra
 * viajes. Los viajes se declaran en el cuaderno de la pantalla de la cabina, y
 * es ese inicio el que pone los contadores a cero (/api/viaje -> op_inicio).
 * Antes habia aqui un "Inicio"/"Finalizar" y un aviso al arrancar, o sea un
 * segundo viaje en paralelo que nadie sincronizaba: si no te acordabas de
 * pulsarlo, el resumen.txt del viaje se llevaba la energia del anterior.
 *
 * Lo que queda: mirar los numeros, ponerlos a cero suelto si hace falta, y
 * soltar la tarjeta para poder sacarla (que no tiene nada que ver con el
 * viaje, pero es el unico sitio donde estaba).
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "fonts/fonts_es.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lvgl.h>
#include "esp_log.h"
#include "data/trip_computer.h"
#include "battery_history.h"
#include "data/solar_daily.h"
#include "ne185_vlog.h"
#include "datalogger.h"

static lv_obj_t *s_trip_label = NULL;

/* ── Energia del viaje: refresco periodico y reset ─────────────────── */
void trip_label_refresh(void)
{
    if (!s_trip_label || !lv_obj_is_visible(s_trip_label)) return;
    trip_computer_t t;
    trip_computer_get(&t);
    char start_str[24] = "--";
    if (t.reset_epoch > 0) {
        struct tm tm_l;
        localtime_r((time_t *)&t.reset_epoch, &tm_l);
        strftime(start_str, sizeof(start_str), "%d/%m %H:%M", &tm_l);
    }
    /* Tiempo de viaje = reloj transcurrido desde la puesta a cero, que
     * normalmente es el inicio del viaje declarado en la cabina (siempre
     * avanza, aunque no llegue telemetria del BMV). */
    int64_t elapsed = 0;
    if (t.reset_epoch > 0) {
        time_t now = time(NULL);
        if (now >= (time_t)t.reset_epoch) elapsed = (int64_t)now - t.reset_epoch;
    }
    /* Los viajes duran dias: "130h 18m" no se lee de un vistazo. Los dias solo
     * aparecen cuando los hay, para que un viaje recien empezado no ensene
     * un "0d" que no aporta nada. */
    int e_days  = (int)(elapsed / 86400);
    int hours   = (int)((elapsed % 86400) / 3600);
    int minutes = (int)((elapsed % 3600) / 60);
    char elapsed_str[24];
    if (e_days > 0) {
        snprintf(elapsed_str, sizeof(elapsed_str), "%dd %dh %02dm",
                 e_days, hours, minutes);
    } else {
        snprintf(elapsed_str, sizeof(elapsed_str), "%dh %02dm", hours, minutes);
    }

    /* Medias solares por dia (proyeccion: divide por los dias exactos
     * transcurridos, aunque sean horas). Guardamos contra division por cero. */
    double days = (elapsed > 0) ? (double)elapsed / 86400.0 : 0.0;
    double solar_h_day  = (days > 0.0) ? (t.solar_seconds / 3600.0) / days : 0.0;
    double solar_ah_day = (days > 0.0) ? t.ah_solar / days : 0.0;

    char buf[288];
    snprintf(buf, sizeof(buf),
        "Iniciado %s   |   %s\n"
        "Total cargado: %.2f kWh %.1f Ah  "
        "(Solar: %.2f kWh %.1f Ah, %.1f h/dia, %.0f Ah/dia)\n"
        "Consumido: %.2f kWh  %.1f Ah",
        start_str, elapsed_str,
        t.wh_charged / 1000.0, t.ah_charged,
        t.wh_solar / 1000.0, t.ah_solar, solar_h_day, solar_ah_day,
        t.wh_discharged / 1000.0, t.ah_discharged);
    lv_label_set_text(s_trip_label, buf);
}

static void do_trip_reset_action(void)
{
    trip_computer_reset();
    trip_label_refresh();
}

static void trip_reset_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_WARNING "  Energía del viaje",
        "Poner los contadores a cero?\n\n"
        "Normalmente no hace falta: se ponen solos al\n"
        "empezar un viaje en la pantalla de la cabina.",
        "Poner a cero", do_trip_reset_action);
}

/* Soltar la tarjeta: se vuelca TODO lo que hay pendiente en memoria y se
 * desmonta, para poder sacarla sin corromper nada. Despues ya no se escribe mas
 * hasta reiniciar; el aviso final lo deja claro.
 *
 * No cierra el viaje -- eso lo hace el cuaderno de la cabina. Aqui solo se
 * guardan los contadores (flush) por si se corta la corriente. */
static void do_soltar_tarjeta_action(void)
{
    ESP_LOGI(TAG_SETTINGS, "Soltar tarjeta: volcando todo");
    battery_history_flush();     /* historico de corriente/tension/panel */
    solar_daily_flush();         /* dia de produccion en curso */
    ne185_vlog_flush();          /* comparativa de voltaje NE185 (hasta 10 min en RAM) */
    trip_computer_flush();       /* contadores a NVS (el viaje lo cierra la cabina) */

    const esp_err_t err = datalogger_close_sd();   /* incluye su propio flush */
    if (err == ESP_OK) {
        ui_show_info_dialog(LV_SYMBOL_SD_CARD "  Tarjeta suelta",
            "Todo guardado.\n\nYa puedes sacar la tarjeta.\n\n"
            "Para volver a registrar, reinicia la pantalla.");
    } else {
        ui_show_info_dialog(LV_SYMBOL_WARNING "  Tarjeta ocupada",
            "Se ha guardado todo lo pendiente, pero la tarjeta\n"
            "no se ha podido soltar (puede estar ocupada).\n\n"
            "Espera unos segundos y vuelve a intentarlo.");
    }
}

static void trip_finish_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_SD_CARD "  Soltar tarjeta",
        "Se guarda todo y se suelta la tarjeta\npara poder sacarla.\n\n"
        "Despues no se registra nada mas\nhasta reiniciar la pantalla.",
        "Soltar", do_soltar_tarjeta_action);
}

void create_trip_card(lv_obj_t *cont)
{
    /* Energia del viaje: contadores reseteables del viaje. */
    lv_obj_t *card_trip = lv_obj_create(cont);
    lv_obj_set_width(card_trip, lv_pct(100));
    lv_obj_set_height(card_trip, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_trip, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_trip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_trip, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_border_width(card_trip, 1, 0);
    lv_obj_set_style_radius(card_trip, 12, 0);
    lv_obj_set_style_pad_hor(card_trip, 16, 0);
    lv_obj_set_style_pad_ver(card_trip, 8, 0);     /* menos alto: menos relleno arriba/abajo */
    lv_obj_set_style_pad_gap(card_trip, 4, 0);
    lv_obj_set_layout(card_trip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_trip, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *trip_head = lv_obj_create(card_trip);
    lv_obj_remove_style_all(trip_head);
    lv_obj_set_size(trip_head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_head, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trip_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *trip_title = lv_label_create(trip_head);
    lv_obj_set_style_text_font(trip_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(trip_title, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(trip_title, LV_SYMBOL_REFRESH "  Energía del viaje");

    /* Dos botones sueltos: poner los contadores a cero (normalmente lo hace
     * solo el inicio de viaje de la cabina) y soltar la tarjeta. */
    lv_obj_t *trip_btns = lv_obj_create(trip_head);
    lv_obj_remove_style_all(trip_btns);
    lv_obj_set_size(trip_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(trip_btns, 8, 0);

    lv_obj_t *btn_trip_rst = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_rst, 190, 44);
    lv_obj_set_style_bg_color(btn_trip_rst, lv_color_hex(0x00897B), 0);
    lv_obj_set_style_radius(btn_trip_rst, 8, 0);
    lv_obj_t *lbl_trip_rst = lv_label_create(btn_trip_rst);
    lv_label_set_text(lbl_trip_rst, "Poner a cero");
    lv_obj_set_style_text_font(lbl_trip_rst, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_rst);
    lv_obj_add_event_cb(btn_trip_rst, trip_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_trip_fin = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_fin, 190, 44);
    lv_obj_set_style_bg_color(btn_trip_fin, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_radius(btn_trip_fin, 8, 0);
    lv_obj_t *lbl_trip_fin = lv_label_create(btn_trip_fin);
    lv_label_set_text(lbl_trip_fin, "Soltar tarjeta");
    lv_obj_set_style_text_font(lbl_trip_fin, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_fin);
    lv_obj_add_event_cb(btn_trip_fin, trip_finish_btn_cb, LV_EVENT_CLICKED, NULL);

    s_trip_label = lv_label_create(card_trip);
    lv_obj_set_style_text_font(s_trip_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_trip_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(s_trip_label, lv_pct(100));
    /* LONG_DOT en vez de WRAP por riesgo WDT al construir. */
    lv_label_set_long_mode(s_trip_label, LV_LABEL_LONG_DOT);
    trip_label_refresh();
}
