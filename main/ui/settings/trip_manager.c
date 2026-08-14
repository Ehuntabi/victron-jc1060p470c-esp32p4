/* Trip computer: tarjeta de Ajustes (Autocaravana), refresco periodico,
 * reset/finalizar y el aviso de arranque "Nuevo viaje?".
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver
 * settings_common.h). Es el mismo codigo movido de sitio, sin cambios de
 * comportamiento.
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

/* ── Trip computer: refresco periodico y reset ─────────────────── */
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
    /* Tiempo de viaje = reloj transcurrido desde el ultimo "Nuevo viaje"
     * (siempre avanza, aunque no llegue telemetria del BMV). */
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
    ui_show_confirm_dialog(LV_SYMBOL_WARNING "  Trip computer",
        "Empezar un viaje nuevo?\nLos contadores vuelven a cero.",
        "Empezar", do_trip_reset_action);
}

/* Cierre de viaje: se vuelca a la tarjeta TODO lo que hay pendiente en memoria y
 * se desmonta, para poder sacarla sin corromper nada. Despues ya no se escribe
 * mas hasta reiniciar; el aviso final lo deja claro. */
static void do_trip_finish_action(void)
{
    ESP_LOGI(TAG_SETTINGS, "Finalizar viaje: volcando todo a la tarjeta");
    battery_history_flush();     /* historico de corriente/tension/panel */
    solar_daily_flush();         /* dia de produccion en curso */
    ne185_vlog_flush();          /* comparativa de voltaje NE185 (hasta 10 min en RAM) */
    trip_computer_end();         /* guarda los contadores Y cierra el viaje */

    const esp_err_t err = datalogger_close_sd();   /* incluye su propio flush */
    if (err == ESP_OK) {
        ui_show_info_dialog(LV_SYMBOL_SD_CARD "  Viaje finalizado",
            "Todo guardado.\n\nYa puedes sacar la tarjeta.\n\n"
            "Para volver a registrar, reinicia la pantalla.");
    } else {
        ui_show_info_dialog(LV_SYMBOL_WARNING "  Viaje finalizado",
            "Se ha guardado todo lo pendiente, pero la tarjeta\n"
            "no se ha podido soltar (puede estar ocupada).\n\n"
            "Espera unos segundos y vuelve a intentarlo.");
    }
}

static void trip_finish_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_SD_CARD "  Finalizar viaje",
        "Se guarda todo y se suelta la tarjeta\npara poder sacarla.\n\n"
        "Despues no se registra nada mas\nhasta reiniciar la pantalla.",
        "Finalizar", do_trip_finish_action);
}

void create_trip_card(lv_obj_t *cont)
{
    /* Trip computer: contadores reseteables del viaje. */
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
    lv_label_set_text(trip_title, LV_SYMBOL_REFRESH "  Trip computer");

    /* Dos botones: empezar viaje (pone a cero) y finalizarlo (guarda y suelta
     * la tarjeta). Van juntos porque son las dos acciones del viaje. */
    lv_obj_t *trip_btns = lv_obj_create(trip_head);
    lv_obj_remove_style_all(trip_btns);
    lv_obj_set_size(trip_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(trip_btns, 8, 0);

    lv_obj_t *btn_trip_rst = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_rst, 140, 44);
    lv_obj_set_style_bg_color(btn_trip_rst, lv_color_hex(0x00897B), 0);
    lv_obj_set_style_radius(btn_trip_rst, 8, 0);
    lv_obj_t *lbl_trip_rst = lv_label_create(btn_trip_rst);
    lv_label_set_text(lbl_trip_rst, "Inicio");
    lv_obj_set_style_text_font(lbl_trip_rst, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_rst);
    lv_obj_add_event_cb(btn_trip_rst, trip_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_trip_fin = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_fin, 150, 44);
    lv_obj_set_style_bg_color(btn_trip_fin, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_trip_fin, 8, 0);
    lv_obj_t *lbl_trip_fin = lv_label_create(btn_trip_fin);
    lv_label_set_text(lbl_trip_fin, "Finalizar");
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

/* ── Aviso de arranque "Nuevo viaje?" ─────────────────────────────
 * Misma estetica que ui_show_confirm_dialog, pero con dos botones
 * explicitos: "Seguir viaje" (no toca nada) y "Nuevo viaje" (resetea).
 * No reutiliza ui_show_confirm_dialog porque ese detecta el boton por el
 * texto "Cancelar"; aqui queremos etiquetas propias. */
static lv_obj_t *s_newtrip_modal = NULL;

static void newtrip_close(void)
{
    if (s_newtrip_modal) { lv_obj_del(s_newtrip_modal); s_newtrip_modal = NULL; }
}

static void newtrip_keep_cb(lv_event_t *e)
{
    (void)e;
    /* "Seguir viaje" tambien abre el viaje: si no, el aviso volveria a salir en
     * el siguiente arranque preguntando lo mismo. */
    trip_computer_mark_active();
    newtrip_close();
}

static void newtrip_reset_cb(lv_event_t *e)
{
    (void)e;
    trip_computer_reset();
    trip_label_refresh();   /* seguro aunque la card aun no exista (chequea s_trip_label) */
    newtrip_close();
}

void ui_show_new_trip_dialog(void)
{
    if (s_newtrip_modal) return;

    /* Fondo modal a pantalla completa (identico al resto de dialogos). */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_newtrip_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0x00C851), 0);  /* verde: viaje */
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(title, LV_SYMBOL_REFRESH "  Nuevo viaje");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Empezar un viaje nuevo?\nSe pondran a cero los contadores del viaje.");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_keep = lv_btn_create(row_btns);
    lv_obj_set_size(btn_keep, 240, 60);
    lv_obj_set_style_bg_color(btn_keep, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_keep, 12, 0);
    lv_obj_t *lk = lv_label_create(btn_keep);
    lv_label_set_text(lk, "Seguir viaje");
    lv_obj_set_style_text_font(lk, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lk);
    lv_obj_add_event_cb(btn_keep, newtrip_keep_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_new = lv_btn_create(row_btns);
    lv_obj_set_size(btn_new, 240, 60);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn_new, 12, 0);
    lv_obj_t *ln = lv_label_create(btn_new);
    lv_label_set_text(ln, LV_SYMBOL_REFRESH "  Nuevo viaje");
    lv_obj_set_style_text_font(ln, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ln, lv_color_hex(0x0A0A0A), 0);  /* texto oscuro sobre verde */
    lv_obj_center(ln);
    lv_obj_add_event_cb(btn_new, newtrip_reset_cb, LV_EVENT_CLICKED, NULL);
}
