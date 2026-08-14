#include "ui.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "portal/config_server.h"
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <lvgl.h>
#include "esp_lvgl_port.h"  // lv_port.h sustituido por esp_lvgl_port
#include "esp_log.h"
#include "esp_wifi.h"
#include "victron_ble.h"
#include "battery_history.h"
#include "alerts.h"
#include "victron_products.h"
#include "ui/views/frigo_panel.h"
#include "ui/vigilancia/gallery.h"
#include "ne185/ne185.h"
#include "nvs_flash.h"
#include "config_storage.h"
#include <stdio.h>
#include "ui/widgets/ui_state.h"
#include "ui/views/device_view.h"
#include "ui/views/view_registry.h"
#include "ui/settings/settings_panel.h"
#include "ui/views/view_default_battery.h"
#include "ui/views/view_overview.h"
#include "rtc_rx8025t.h"
#include "datalogger.h"
#include "data/dashboard_state.h"
#include "data/trip_computer.h"
#include "data/solar_daily.h"
#include "log_browser.h"
#include "screenshot.h"
#include "frigo.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "chart_common.h"

/* ── Pantalla gráfica temperaturas ─────────────────────────── */
static lv_obj_t *s_chart_screen = NULL;  /* overlay raíz */
static lv_obj_t *s_chart      = NULL;     /* widget chart interno */
static lv_chart_series_t *s_ser_aletas     = NULL;
static lv_chart_series_t *s_ser_congelador = NULL;
static lv_chart_series_t *s_ser_exterior   = NULL;
static lv_chart_series_t *s_ser_fan        = NULL;
/* Banda del excedente solar: comparte el eje secundario (0..100) con s_ser_fan
 * porque el proyecto fija ese rango una sola vez al crear el chart
 * (lv_chart_set_range en ui_show_chart_screen); no se puede pedir un rango
 * 0..1 propio sin romper la escala del fan%. Se dibuja como marca baja
 * (valor bajo del 0..100) solo en las muestras activas. */
static lv_chart_series_t *s_ser_solar      = NULL;
/* Leyenda-boton: cada elemento muestra/oculta su serie. Guardamos etiqueta,
 * punto y color para poder atenuarlos al ocultar. Estado por sesion de pantalla
 * (las series se recrean al abrir -> todo visible por defecto). */
static lv_obj_t   *s_frigo_leg_lbl[4] = {NULL};
static lv_obj_t   *s_frigo_leg_dot[4] = {NULL};
static lv_color_t  s_frigo_leg_col[4];
static bool        s_frigo_ser_hidden[4] = {false};
static lv_obj_t *s_frigo_lbl_date = NULL;    /* header con la fecha */
static lv_obj_t *s_frigo_xlabels = NULL;     /* contenedor de etiquetas hora */
static lv_obj_t *s_frigo_lbl_zoom = NULL;
static int  s_frigo_day_idx = -1;            /* -1 = "hoy" buffer RAM */
static int  s_frigo_n_dates = 0;
static char s_frigo_dates[LOG_BROWSER_MAX_DATES][LOG_BROWSER_DATE_LEN];
/* Ventana [a, b) en fraccion 0..1 sobre los datos del dia */
static float s_frigo_win_a = 0.0f;
static float s_frigo_win_b = 1.0f;

/* Estado del gesto tactil (arrastrar = pan, doble-toque = zoom, manten = 1x) */
static int32_t s_frigo_drag_last_x  = 0;
static int32_t s_frigo_drag_moved   = 0;
static bool    s_frigo_dragging     = false;
static int64_t s_frigo_last_apply_us = 0;
static int64_t s_frigo_last_click_us = 0;

/* Buffer para parsear el CSV de un dia guardado (1440 muestras = 1 min c/u).
 * En PSRAM (lazy): 1500 x 24 B ~= 36 KB, demasiado para RAM interna estatica.
 * Se reserva al ver un dia historico y se libera al cerrar la grafica. */
#define FRIGO_LOG_MAX_ENTRIES   1500
static frigo_log_entry_t *s_frigo_buf = NULL;
/* Cache: dia (idx) ya parseado en s_frigo_buf y su n. Evita re-leer/re-parsear
 * el CSV de la SD en cada tick de pan/zoom (apply_window). -2 = cache vacia. */
static int s_frigo_loaded_idx = -2;
static int s_frigo_loaded_n   = 0;

static void frigo_chart_load_day(void);
static void frigo_chart_gesture_cb(lv_event_t *e);
static void frigo_arrow_cb(lv_event_t *e);
static void frigo_chart_touch_cb(lv_event_t *e);
static void frigo_legend_toggle_cb(lv_event_t *e);
static void frigo_apply_window(void);
static void frigo_update_zoom_label(void);

static void chart_screen_close_cb(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    lv_obj_del(screen);
    s_chart_screen = NULL;
    s_chart = NULL;
}



/* OJO: el CSV de hoy TIENE que seguir en la lista de dias navegables. Parece
 * redundante con la vista "HOY", pero no lo es: "HOY" sale del buffer de RAM,
 * que arranca VACIO en cada reinicio y solo guarda ~16 h. El fichero de la SD es
 * la unica copia del dia que sobrevive a un arranque. Filtrarlo (v1.4.1) dejo
 * los dos historicos vacios tras instalar una actualizacion. */

void ui_show_chart_screen(ui_state_t *ui)
{
    if (!ui) return;
    /* Guard anti doble-apertura: sin esto un segundo tap deja el overlay
     * anterior huerfano (fuga de pantalla LVGL entera) y los s_* colgantes.
     * Mismo patron que ui_show_battery_history_screen. */
    if (s_chart_screen) return;

    /* Crear pantalla a pantalla completa */
    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_move_foreground(scr);
    s_chart_screen = scr;

    /* Título */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_label_set_text(lbl_title, "Temperaturas");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 16, 12);

    /* Fecha del log mostrado (centrada arriba) */
    s_frigo_lbl_date = lv_label_create(scr);
    lv_obj_set_style_text_font(s_frigo_lbl_date, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_frigo_lbl_date, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_frigo_lbl_date, "HOY");
    lv_obj_align(s_frigo_lbl_date, LV_ALIGN_TOP_MID, 0, 12);

    /* Flechas PULSABLES a izq/dcha de la fecha. Antes eran solo decorativas
     * ("para indicar swipe"): parecian controles y no hacian nada al tocarlas. */
    lv_obj_t *lbl_arr_l = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_l, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_l, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_l, LV_SYMBOL_LEFT);
    lv_obj_align(lbl_arr_l, LV_ALIGN_TOP_MID, -120, 14);
    lv_obj_add_flag(lbl_arr_l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_arr_l, 30);
    lv_obj_add_event_cb(lbl_arr_l, frigo_arrow_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *lbl_arr_r = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_r, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_r, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_r, LV_SYMBOL_RIGHT);
    lv_obj_align(lbl_arr_r, LV_ALIGN_TOP_MID, 120, 14);
    lv_obj_add_flag(lbl_arr_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_arr_r, 30);
    lv_obj_add_event_cb(lbl_arr_r, frigo_arrow_cb, LV_EVENT_CLICKED, NULL);

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, chart_screen_close_cb, LV_EVENT_CLICKED, scr);

    /* Leyenda */
    const char *leyenda[] = {"Aletas", "Congel.", "Exter.", "Fan%"};
    lv_color_t colores[]  = {
        lv_color_hex(0x00BFFF),
        lv_color_hex(0xFF4444),
        lv_color_hex(0x44FF44),
        lv_color_hex(0xFFAA00)
    };
    for (int i = 0; i < 4; i++) {
        s_frigo_leg_col[i]    = colores[i];
        s_frigo_ser_hidden[i] = false;

        /* Cada elemento de la leyenda es un boton: al tocarlo se muestra/oculta
         * su linea en la grafica. El contenedor transparente es el area tactil. */
        lv_obj_t *item = lv_obj_create(scr);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, 150, 34);
        /* Grupo centrado: 4 items de paso 165 ocupan (3*165 + 150) px. */
        lv_obj_set_pos(item, (LV_HOR_RES - (3 * 165 + 150)) / 2 + i * 165, 560);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(item, frigo_legend_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(item);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_bg_color(dot, colores[i], 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);  /* el toque lo recibe el item */

        lv_obj_t *lbl = lv_label_create(item);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(lbl, colores[i], 0);
        lv_label_set_text(lbl, leyenda[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 24, 0);

        s_frigo_leg_dot[i] = dot;
        s_frigo_leg_lbl[i] = lbl;
    }

    s_frigo_win_a = 0.0f;
    s_frigo_win_b = 1.0f;

    /* Etiqueta de nivel de zoom (derecha, bajo el boton Cerrar) */
    s_frigo_lbl_zoom = lv_label_create(scr);
    lv_obj_set_style_text_color(s_frigo_lbl_zoom, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_frigo_lbl_zoom, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(s_frigo_lbl_zoom, "1x");
    lv_obj_align(s_frigo_lbl_zoom, LV_ALIGN_TOP_RIGHT, -16, 80);

    /* Pista de uso tactil (zoom/pan sin botones) */
    {
        lv_obj_t *hint = lv_label_create(scr);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8A93A6), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14_es, 0);
        lv_label_set_text(hint,
            "Arrastra: mover  -  2 toques: zoom  -  manten: 1x");
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, 84);
    }

    /* Gráfica */
    s_chart = lv_chart_create(scr);
    /* Estrechado para dejar margen a las etiquetas de los dos ejes Y (LVGL las
     * dibuja fuera del borde del chart): temp a la izquierda, fan% a la derecha. */
    lv_obj_set_size(s_chart, LV_HOR_RES - 150, LV_VER_RES - 186);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart, frigo_chart_touch_cb, LV_EVENT_ALL, NULL);
    /* Sin esto el gesto de deslizar MUERE en la grafica: LVGL lo entrega al
     * objeto tocado y solo sube al padre con EVENT_BUBBLE. Como la grafica ocupa
     * casi toda la pantalla, deslizar donde es natural no cambiaba de dia. */
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(s_chart, 5, 10);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x333333), LV_PART_MAIN);

    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y,   8, 4, 5, 1, true, 60);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_SECONDARY_Y, 8, 4, 5, 1, true, 60);
    lv_obj_set_style_pad_left(s_chart, 8, 0);
    lv_obj_set_style_pad_right(s_chart, 8, 0);
    /* Hueco arriba para que la etiqueta Y superior (fuente 20) no se recorte. */
    lv_obj_set_style_pad_top(s_chart, 16, 0);
    lv_obj_set_style_text_color(s_chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(s_chart, &lv_font_montserrat_20_es, LV_PART_TICKS);

    s_ser_aletas     = lv_chart_add_series(s_chart, colores[0], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_congelador = lv_chart_add_series(s_chart, colores[1], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_exterior   = lv_chart_add_series(s_chart, colores[2], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_fan        = lv_chart_add_series(s_chart, colores[3], LV_CHART_AXIS_SECONDARY_Y);
    s_ser_solar      = lv_chart_add_series(s_chart, lv_color_hex(0xE0900A), LV_CHART_AXIS_SECONDARY_Y);

    /* Contenedor de labels horarios bajo el chart */
    s_frigo_xlabels = lv_obj_create(scr);
    lv_obj_remove_style_all(s_frigo_xlabels);
    lv_obj_set_size(s_frigo_xlabels, LV_HOR_RES - 150, 28);
    lv_obj_align_to(s_frigo_xlabels, s_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_layout(s_frigo_xlabels, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_frigo_xlabels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_frigo_xlabels, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_label_create(s_frigo_xlabels);
        lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_text(l, "--:--");
    }

    /* Inicializar navegacion: listar fechas SD y mostrar HOY */
    s_frigo_n_dates = log_browser_list_dates("/sdcard/frigo",
                                             s_frigo_dates, LOG_BROWSER_MAX_DATES);
    if (s_frigo_n_dates > 0) {
        char today[11];
        today_ymd(today);
        if (strcmp(s_frigo_dates[s_frigo_n_dates - 1], today) == 0)
            s_frigo_n_dates--;
    }
    s_frigo_day_idx = -1;
    s_frigo_loaded_idx = -2;   /* re-listado de fechas: invalidar cache del CSV */
    frigo_chart_load_day();

    /* Gestures para navegar entre dias */
    lv_obj_add_event_cb(scr, frigo_chart_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* Toca un elemento de la leyenda -> muestra/oculta su serie y atenua la etiqueta. */
static void frigo_legend_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 4 || !s_chart) return;
    lv_chart_series_t *ser = (i == 0) ? s_ser_aletas
                           : (i == 1) ? s_ser_congelador
                           : (i == 2) ? s_ser_exterior
                                      : s_ser_fan;
    if (!ser) return;
    bool hide = !s_frigo_ser_hidden[i];
    s_frigo_ser_hidden[i] = hide;
    lv_chart_hide_series(s_chart, ser, hide);
    if (s_frigo_leg_lbl[i])
        lv_obj_set_style_text_color(s_frigo_leg_lbl[i],
            hide ? lv_color_hex(0x555555) : s_frigo_leg_col[i], 0);
    if (s_frigo_leg_dot[i])
        lv_obj_set_style_bg_opa(s_frigo_leg_dot[i],
            hide ? LV_OPA_30 : LV_OPA_COVER, 0);
}

/* `base` = indice global de la primera muestra de HOY, `n` = cuantas hay. Los
 * indices que se pasan a datalogger_get_entry siguen siendo globales. */
static void update_frigo_xlabels_today(int base, int n)
{
    if (!s_frigo_xlabels) return;
    if (n <= 0) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
            if (l) lv_label_set_text(l, "--:--");
        }
        return;
    }
    int a = (int)(s_frigo_win_a * n);
    int b = (int)(s_frigo_win_b * n);
    if (b <= a) b = a + 1;
    if (b > n) b = n;
    int wn = b - a;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
        if (!l) continue;
        int idx = base + a + (wn - 1) * i / 4;
        if (idx < base) idx = base;
        if (idx >= base + n) idx = base + n - 1;
        const datalogger_entry_t *e = datalogger_get_entry(idx);
        if (!e) { lv_label_set_text(l, "--:--"); continue; }
        const char *ts = e->timestamp;
        char buf[8] = {0};
        if (strncmp(ts, "BOOT", 4) == 0 && strlen(ts) >= 10) {
            strncpy(buf, ts + 5, 5);
        } else if (strlen(ts) >= 16) {
            strncpy(buf, ts + 11, 5);
        } else {
            buf[0] = 0;
        }
        lv_label_set_text(l, buf[0] ? buf : "--:--");
    }
}

static void update_frigo_xlabels_from_buf(int n)
{
    if (!s_frigo_xlabels) return;
    if (n <= 0) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
            if (l) lv_label_set_text(l, "--:--");
        }
        return;
    }
    int a = (int)(s_frigo_win_a * n);
    int b = (int)(s_frigo_win_b * n);
    if (b <= a) b = a + 1;
    if (b > n) b = n;
    int wn = b - a;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
        if (!l) continue;
        int idx = a + (wn - 1) * i / 4;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        lv_label_set_text_fmt(l, "%02d:%02d",
                              s_frigo_buf[idx].hh, s_frigo_buf[idx].mm);
    }
}

static void frigo_apply_temp_range(float t_min, float t_max)
{
    if (t_min > t_max) { t_min = -20.0f; t_max = 15.0f; }
    float span = t_max - t_min;
    if (span < 1.0f) span = 1.0f;
    int y_min = (int)(t_min - span * 0.05f);
    int y_max = (int)(t_max + span * 0.05f) + 1;
    if (y_min == y_max) { y_min--; y_max++; }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);
}

/* Indice global de la primera muestra de HOY en el buffer del datalogger. Las
 * muestras estan en orden cronologico, asi que las de hoy son el tramo final.
 * Devuelve `count` si no hay ninguna de hoy, y 0 si el reloj aun no tiene hora
 * (entonces se muestra todo, que es lo unico util sin fecha). */
static int frigo_today_base(int count)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year <= 100) return 0;
    char today[11];
    snprintf(today, sizeof today, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    for (int i = 0; i < count; ++i) {
        const datalogger_entry_t *e = datalogger_get_entry(i);
        /* NULL = no se consiguio el mutex del datalogger (hay un volcado a la SD
         * en curso), NO "esta muestra no es de hoy". Tratarlo como descarte
         * vaciaba la grafica entera si el volcado pillaba el barrido. Ante la
         * duda, mostrar todo el buffer: es el comportamiento de siempre. */
        if (!e) return 0;
        if (strncmp(e->timestamp, today, 10) == 0) return i;
    }
    return count;
}

/* Pega al final de `buf` (que ya trae el CSV de hoy) las muestras del anillo
 * del datalogger posteriores a la ultima que trajo el CSV, y actualiza `n`.
 *
 * Mismo motivo que bh_append_ring_tail (bateria, mas abajo): el CSV llega
 * hasta el ultimo volcado (cada 60 s) pero SOBREVIVE a los reinicios, y el
 * anillo tiene el minuto en curso pero arranca vacio en cada arranque. Juntos
 * dan el dia entero; solo el CSV o solo el anillo dejan el dia partido en dos
 * vistas ("HOY" con lo de despues del reinicio, la fecha de hoy navegable con
 * lo de antes). */
static void frigo_append_ring_tail(frigo_log_entry_t *buf, int *n, int max)
{
    char today[11];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(today, sizeof today, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);

    int last_min = (*n > 0) ? buf[*n - 1].hh * 60 + buf[*n - 1].mm : -1;
    int count = datalogger_get_count();
    for (int i = 0; i < count && *n < max; ++i) {
        const datalogger_entry_t *e = datalogger_get_entry(i);
        if (!e) continue;
        if (strncmp(e->timestamp, today, 10) != 0) continue;   /* no es de hoy */
        int hh = 0, mm = 0;
        if (sscanf(e->timestamp + 11, "%d:%d", &hh, &mm) != 2) continue;
        if (hh * 60 + mm <= last_min) continue;   /* ya venia en el CSV */
        frigo_log_entry_t *o = &buf[*n];
        o->hh = hh;
        o->mm = mm;
        o->t_aletas = e->T_Aletas;
        o->t_congel = e->T_Congelador;
        o->t_exter  = e->T_Exterior;
        o->fan_pct  = e->fan_percent;
        o->excedente_solar = e->excedente_solar ? 1 : 0;
        (*n)++;
    }
}

static void frigo_chart_load_day(void)
{
    if (!s_chart) return;
    /* Resetear todas las series e indice circular */
    lv_chart_set_all_value(s_chart, s_ser_aletas,     LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_congelador, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_exterior,   LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_fan,        LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_solar,      LV_CHART_POINT_NONE);

    time_t frigo_now = time(NULL);
    struct tm frigo_lt;
    localtime_r(&frigo_now, &frigo_lt);
    bool frigo_clock_ok = frigo_lt.tm_year > 100;

    if (s_frigo_day_idx < 0 && !frigo_clock_ok) {
        /* Reloj sin hora aun: no hay como nombrar el CSV de hoy ni fecharlo
         * (BOOT+HH:MM:SS en vez de fecha real). Mostrar el anillo entero sin
         * fusionar con la SD, que es lo unico util sin fecha. */
        int count = datalogger_get_count();
        int base  = frigo_today_base(count);
        int n     = count - base;
        if (n < 2) { n = 2; base = count - 2; if (base < 0) base = 0; }
        int wa = base + (int)(s_frigo_win_a * n);
        int wb = base + (int)(s_frigo_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > base + n) wb = base + n;
        int wn = wb - wa;
        if (wn < 2) wn = 2;
        lv_chart_set_point_count(s_chart, wn);
        float t_min = 9999.0f, t_max = -9999.0f;
        int valid = 0;
        for (int i = wa; i < wb; i++) {
            const datalogger_entry_t *e = datalogger_get_entry(i);
            if (!e) continue;
            valid++;
            int idx = i - wa;
            if (e->T_Aletas     > -120.0f) { if (e->T_Aletas     < t_min) t_min = e->T_Aletas;     if (e->T_Aletas     > t_max) t_max = e->T_Aletas; }
            if (e->T_Congelador > -120.0f) { if (e->T_Congelador < t_min) t_min = e->T_Congelador; if (e->T_Congelador > t_max) t_max = e->T_Congelador; }
            if (e->T_Exterior   > -120.0f) { if (e->T_Exterior   < t_min) t_min = e->T_Exterior;   if (e->T_Exterior   > t_max) t_max = e->T_Exterior; }
            lv_chart_set_value_by_id(s_chart, s_ser_aletas, idx,
                e->T_Aletas > -120.0f ? (int16_t)e->T_Aletas : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_congelador, idx,
                e->T_Congelador > -120.0f ? (int16_t)e->T_Congelador : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_exterior, idx,
                e->T_Exterior > -120.0f ? (int16_t)e->T_Exterior : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_fan, idx, e->fan_percent);
            lv_chart_set_value_by_id(s_chart, s_ser_solar, idx,
                e->excedente_solar ? 3 : LV_CHART_POINT_NONE);
        }
        frigo_apply_temp_range(t_min, t_max);
        /* Pasamos el rango raw del datalogger (base + n), no `valid`: las
         * funciones de xlabels indexan en datalogger_get_entry(idx) que
         * usa el espacio global de indices; pasar `valid` desincroniza
         * los timestamps del rango visible (zoom al 50-100% mostraba
         * etiquetas del 0-50%). El "--:--" ocasional con datalogger
         * vacio es un bug menor que las etiquetas con la hora incorrecta. */
        update_frigo_xlabels_today(base, n);
        if (s_frigo_lbl_date) lv_label_set_text(s_frigo_lbl_date, "HOY");
        (void)valid;
    } else {
        /* HOY (idx<0, reloj en hora) fusiona el CSV de hoy con la cola del
         * anillo, igual que hace la bateria (bh_loader_task, mas abajo): el
         * CSV sobrevive a los reinicios pero solo llega hasta el ultimo
         * volcado (cada 60 s), y el anillo tiene el minuto en curso pero
         * arranca vacio en cada arranque. Sin esto, HOY solo ensenaba lo de
         * despues del ultimo reinicio y el resto del dia solo se veia
         * navegando a la fecha de hoy como "historico". */
        char date[LOG_BROWSER_DATE_LEN];
        if (s_frigo_day_idx < 0) {
            snprintf(date, sizeof date, "%04d-%02d-%02d",
                     frigo_lt.tm_year + 1900, frigo_lt.tm_mon + 1, frigo_lt.tm_mday);
        } else {
            snprintf(date, sizeof date, "%s", s_frigo_dates[s_frigo_day_idx]);
        }
        if (s_frigo_buf == NULL) {
            s_frigo_buf = heap_caps_malloc(sizeof(frigo_log_entry_t) * FRIGO_LOG_MAX_ENTRIES,
                                           MALLOC_CAP_SPIRAM);
            s_frigo_loaded_idx = -2;   /* buffer nuevo: cache invalida */
        }
        int n;
        if (s_frigo_buf && s_frigo_day_idx == s_frigo_loaded_idx) {
            n = s_frigo_loaded_n;   /* mismo dia ya en s_frigo_buf: no re-leer la SD */
        } else {
            char path[64];
            snprintf(path, sizeof(path), "/sdcard/frigo/%s.csv", date);
            n = s_frigo_buf ? log_browser_load_frigo(path, s_frigo_buf, FRIGO_LOG_MAX_ENTRIES) : 0;
            if (s_frigo_day_idx < 0 && s_frigo_buf)
                frigo_append_ring_tail(s_frigo_buf, &n, FRIGO_LOG_MAX_ENTRIES);
            s_frigo_loaded_idx = s_frigo_buf ? s_frigo_day_idx : -2;
            s_frigo_loaded_n   = n;
        }
        int wa = (int)(s_frigo_win_a * n);
        int wb = (int)(s_frigo_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > n) wb = n;
        int wn = wb - wa;
        /* Mismo tope y downsample que la grafica de bateria (ver
         * ui_show_battery_history_screen): con varias series, un
         * lv_chart_set_point_count grande cuelga taskLVGL > 5 s -> WDT. Aqui hay
         * 5 series y el buffer admite hasta FRIGO_LOG_MAX_ENTRIES (1500); un CSV
         * real son ~288 lineas (log cada 5 min), asi que hoy no se alcanza, pero
         * el tope evita que un cambio de cadencia lo reviva. */
        const int CHART_MAX_PTS = 300;
        int pts = wn > 0 ? wn : 2;
        if (pts > CHART_MAX_PTS) pts = CHART_MAX_PTS;
        if (pts < 2) pts = 2;
        lv_chart_set_point_count(s_chart, pts);
        int step = (wn > CHART_MAX_PTS) ? (wn + CHART_MAX_PTS - 1) / CHART_MAX_PTS : 1;
        float t_min = 9999.0f, t_max = -9999.0f;
        int idx = 0;
        for (int i = wa; i < wb && idx < pts; i += step, ++idx) {
            const frigo_log_entry_t *e = &s_frigo_buf[i];
            if (!isnan(e->t_aletas))  { if (e->t_aletas  < t_min) t_min = e->t_aletas;  if (e->t_aletas  > t_max) t_max = e->t_aletas; }
            if (!isnan(e->t_congel))  { if (e->t_congel  < t_min) t_min = e->t_congel;  if (e->t_congel  > t_max) t_max = e->t_congel; }
            if (!isnan(e->t_exter))   { if (e->t_exter   < t_min) t_min = e->t_exter;   if (e->t_exter   > t_max) t_max = e->t_exter; }
            lv_chart_set_value_by_id(s_chart, s_ser_aletas, idx,
                isnan(e->t_aletas) ? LV_CHART_POINT_NONE : (int16_t)e->t_aletas);
            lv_chart_set_value_by_id(s_chart, s_ser_congelador, idx,
                isnan(e->t_congel) ? LV_CHART_POINT_NONE : (int16_t)e->t_congel);
            lv_chart_set_value_by_id(s_chart, s_ser_exterior, idx,
                isnan(e->t_exter)  ? LV_CHART_POINT_NONE : (int16_t)e->t_exter);
            lv_chart_set_value_by_id(s_chart, s_ser_fan, idx, e->fan_pct);
            lv_chart_set_value_by_id(s_chart, s_ser_solar, idx,
                e->excedente_solar ? 3 : LV_CHART_POINT_NONE);
        }
        frigo_apply_temp_range(t_min, t_max);
        update_frigo_xlabels_from_buf(n);
        if (s_frigo_lbl_date) {
            if (s_frigo_day_idx < 0) {
                lv_label_set_text(s_frigo_lbl_date, "HOY");
            } else {
                char disp[11];
                fmt_date_ddmmaaaa(date, disp, sizeof disp);
                lv_label_set_text(s_frigo_lbl_date, disp);
            }
        }
    }
    lv_chart_refresh(s_chart);
}

static void frigo_update_zoom_label(void)
{
    if (!s_frigo_lbl_zoom) return;
    float w = s_frigo_win_b - s_frigo_win_a;
    if (w <= 0.0f) w = 1.0f;
    float z = 1.0f / w;
    if (z < 1.05f) lv_label_set_text(s_frigo_lbl_zoom, "1x");
    else if (z < 9.5f) lv_label_set_text_fmt(s_frigo_lbl_zoom, "%.1fx", z);
    else               lv_label_set_text_fmt(s_frigo_lbl_zoom, "%dx", (int)(z + 0.5f));
}

static void frigo_apply_window(void)
{
    if (s_frigo_win_a < 0.0f) s_frigo_win_a = 0.0f;
    if (s_frigo_win_b > 1.0f) s_frigo_win_b = 1.0f;
    if (s_frigo_win_b - s_frigo_win_a < 0.01f) {
        float c = (s_frigo_win_a + s_frigo_win_b) * 0.5f;
        s_frigo_win_a = c - 0.005f;
        s_frigo_win_b = c + 0.005f;
        if (s_frigo_win_a < 0) { s_frigo_win_b -= s_frigo_win_a; s_frigo_win_a = 0; }
        if (s_frigo_win_b > 1) { s_frigo_win_a -= (s_frigo_win_b - 1); s_frigo_win_b = 1; }
    }
    frigo_update_zoom_label();
    frigo_chart_load_day();
}

/* Gesto tactil sobre el chart: arrastrar = desplazar (pan), doble-toque =
 * zoom in centrado en el punto, mantener pulsado = volver a 1x. El cambio de
 * dia se hace con swipe (gesture_cb) solo cuando estamos en 1x. */
static void frigo_chart_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_frigo_drag_last_x = p.x;
        s_frigo_drag_moved  = 0;
        s_frigo_dragging    = false;
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dx = p.x - s_frigo_drag_last_x;
        s_frigo_drag_last_x = p.x;
        s_frigo_drag_moved += dx < 0 ? -dx : dx;
        if (s_frigo_drag_moved < 6) return;   /* umbral para no confundir tap */
        s_frigo_dragging = true;
        float win_w = s_frigo_win_b - s_frigo_win_a;
        lv_area_t ca; lv_obj_get_content_coords(s_chart, &ca);
        int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
        float shift = -(float)dx / (float)cw * win_w;
        s_frigo_win_a += shift; s_frigo_win_b += shift;
        if (s_frigo_win_a < 0.0f) { s_frigo_win_b -= s_frigo_win_a; s_frigo_win_a = 0.0f; }
        if (s_frigo_win_b > 1.0f) { s_frigo_win_a -= (s_frigo_win_b - 1.0f); s_frigo_win_b = 1.0f; }
        int64_t now = esp_timer_get_time();
        if (now - s_frigo_last_apply_us > 120000) {  /* throttle recarga ~8/s */
            s_frigo_last_apply_us = now;
            frigo_update_zoom_label();
            frigo_chart_load_day();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_frigo_dragging) {
            frigo_update_zoom_label();
            frigo_chart_load_day();           /* recarga final tras soltar */
            s_frigo_dragging = false;
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (!s_frigo_dragging) {              /* mantener pulsado = reset a 1x */
            s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
            frigo_apply_window();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_frigo_dragging) return;
        int64_t now = esp_timer_get_time();
        if (now - s_frigo_last_click_us < 350000) {   /* doble-toque = zoom in */
            s_frigo_last_click_us = 0;
            float win_w = s_frigo_win_b - s_frigo_win_a;
            lv_area_t ca; lv_obj_get_content_coords(s_chart, &ca);
            int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
            float rel = (float)(p.x - ca.x1) / (float)cw;
            if (rel < 0) rel = 0;
            if (rel > 1) rel = 1;
            float center = s_frigo_win_a + rel * win_w;
            float nw = win_w * 0.5f;
            s_frigo_win_a = center - rel * nw;
            s_frigo_win_b = s_frigo_win_a + nw;
            frigo_apply_window();
        } else {
            s_frigo_last_click_us = now;
        }
    }
}

/* Un dia adelante o atras. `atras` = hacia el pasado. Lo comparten el gesto y
 * los botones de flecha para que no puedan divergir. */
static void frigo_step_day(bool atras)
{
    if (atras) {
        if (s_frigo_day_idx == -1) {
            if (s_frigo_n_dates > 0) s_frigo_day_idx = s_frigo_n_dates - 1;
            else return;
        } else if (s_frigo_day_idx > 0) {
            s_frigo_day_idx--;
        } else {
            return;
        }
    } else {
        if (s_frigo_day_idx < 0) return;
        if (s_frigo_day_idx < s_frigo_n_dates - 1) s_frigo_day_idx++;
        else                                       s_frigo_day_idx = -1;
    }
    s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
    /* Volver a HOY relee: su CSV y el anillo han seguido creciendo mientras
     * se miraban otros dias. Los dias pasados no cambian, asi que esos si
     * valen tal cual desde el buffer (misma logica que bh_step_day). */
    if (s_frigo_day_idx < 0) s_frigo_loaded_idx = -2;
    frigo_update_zoom_label();
    frigo_chart_load_day();
}

static void frigo_arrow_cb(lv_event_t *e)
{
    frigo_step_day(lv_event_get_user_data(e) != NULL);
}

static void frigo_chart_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    /* Zoomeado: el pan se hace arrastrando (frigo_chart_touch_cb); ignoramos
     * el swipe para no cambiar de dia sin querer. */
    bool zoomed = (s_frigo_win_a > 0.0001f) || (s_frigo_win_b < 0.9999f);
    if (zoomed) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT)     frigo_step_day(true);
    else if (dir == LV_DIR_LEFT) frigo_step_day(false);
}

/* Cerrar overlays para rotacion del salvapantallas */
void ui_close_chart_screen(void)
{
    /* Borrar el overlay raíz, que arrastra al chart y todos sus hijos */
    if (s_chart_screen) { lv_obj_del(s_chart_screen); s_chart_screen = NULL; }
    s_chart = NULL;
    /* Liberar el buffer de carga de dias guardados (~36KB PSRAM): se reserva
     * lazy al ver un dia y se re-reserva al volver a abrir. */
    if (s_frigo_buf) { free(s_frigo_buf); s_frigo_buf = NULL; s_frigo_loaded_idx = -2; }
}
