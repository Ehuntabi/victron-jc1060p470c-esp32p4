/* Energia del viaje: tarjeta de Ajustes (Autocaravana) con la energia del viaje.
 *
 * OJO, cambio de fondo el 24-ago-2026: esta tarjeta YA NO abre ni cierra
 * viajes. Los viajes se declaran en el cuaderno de la pantalla de la cabina, y
 * es ese inicio el que pone los contadores a cero (/api/viaje -> op_inicio).
 * Antes habia aqui un "Inicio"/"Finalizar" y un aviso al arrancar, o sea un
 * segundo viaje en paralelo que nadie sincronizaba: si no te acordabas de
 * pulsarlo, el resumen.txt del viaje se llevaba la energia del anterior.
 *
 * 24-sep-2026: FUERA la tarjeta de datos del submenu Autocaravana. Los numeros
 * (cargado, consumido, medias por dia) ya estan en tres sitios -- los CSV del
 * viaje en la SD, la vista Historico solar ("Viaje X kWh") y el portal -- y
 * tenerlos ademas dentro de un menu de AJUSTES era datos en el sitio
 * equivocado. Lo que si era unico eran los dos botones, y cada uno se ha ido a
 * su casa:
 *   - "Poner a cero" (viaje)  -> vista Historico solar, junto al dato del viaje
 *   - "Soltar tarjeta"        -> Ajustes -> Tarjeta SD (de donde salio)
 * Aqui quedan los dos constructores, para que el aspecto y los avisos sean los
 * mismos en los dos sitios (y no haya dos copias que diverjan).
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "ui/widgets/ui_card.h"   /* paleta compartida (UI_COLOR_CARD) */
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

/* ── Reset del viaje y soltar tarjeta ──────────────────────────────── */
static void do_trip_reset_action(void)
{
    /* Los contadores del viaje. La tarjeta que los enseñaba ya no existe: el
     * dato vive en Historico solar y en los CSV del viaje. */
    trip_computer_reset();
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

/* ── Botones, para colocarlos donde toque ────────────────────────────────
 * Devuelven el boton ya hecho (con su aviso de confirmacion), sin padre fijo:
 * quien llame decide donde ponerlo. */
lv_obj_t *trip_reset_button_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 190, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00897B), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Poner a cero");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, trip_reset_btn_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t *trip_eject_button_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 190, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_SD_CARD "  Soltar tarjeta");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, trip_finish_btn_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}
