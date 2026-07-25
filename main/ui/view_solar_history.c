/* Pantalla "Placa Solar" del Historial en graficos.
 *
 * Para que sirve: el usuario tiene ahora una placa de 125 W y en septiembre la
 * dobla (250/300 W). Quiere PODER COMPARAR temporadas, asi que la produccion se
 * guarda dia a dia a largo plazo (ver solar_daily.c).
 *
 * Sigue EL MISMO diseno que la pantalla de Historico de bateria (ui.c):
 * fondo negro, boton Cerrar arriba a la derecha, titulo a la izquierda, rotulo
 * ambar centrado, boton de modo a la izquierda, chart ancho abajo con etiquetas
 * de eje y fila de totales al pie. Si se cambia alli, cambiar aqui.
 *
 * Un solo chart con dos modos:
 *   HOY (24H)  -> potencia del panel a lo largo del dia
 *   POR DIAS   -> barras de produccion contra consumo, un mes de un vistazo
 *
 * Lo que entra a la BATERIA no se repite aqui: eso ya esta en el historico de
 * bateria.
 */
#include "lvgl.h"
#include "ui.h"
#include "ui_state.h"
#include "fonts/fonts_es.h"
#include "battery_history.h"
#include "solar_daily.h"
#include "trip_computer.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SOL_DIAS       30     /* barras visibles en el modo POR DIAS */
#define SOL_CHART_PTS  300    /* mismo tope que el historico de bateria */

static lv_obj_t *s_scr       = NULL;
static lv_obj_t *s_prev      = NULL;
static lv_obj_t *s_chart     = NULL;
static lv_chart_series_t *s_ser_a = NULL;   /* potencia (hoy) / produccion (dias) */
static lv_chart_series_t *s_ser_b = NULL;   /* solo en POR DIAS: consumo */
static lv_obj_t *s_lbl_modo  = NULL;
static lv_obj_t *s_lbl_rotulo = NULL;
static lv_obj_t *s_lbl_hint  = NULL;
static lv_obj_t *s_xlabels   = NULL;
static lv_obj_t *s_totales[4] = { NULL, NULL, NULL, NULL };
static bool      s_modo_dias = false;
/* Las dos primeras etiquetas del pie son la LEYENDA: tocarlas muestra u oculta
 * su serie, igual que en el historico de bateria. Al apagarse se ponen grises. */
static bool      s_oculta[2] = { false, false };
static const uint32_t s_col[2] = { 0xFFD54F, 0x4FC3F7 };

static void sol_legend_toggle_cb(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 1 || !s_chart) return;
    lv_chart_series_t *ser = (i == 0) ? s_ser_a : s_ser_b;
    if (!ser) return;
    s_oculta[i] = !s_oculta[i];
    lv_chart_hide_series(s_chart, ser, s_oculta[i]);
    if (s_totales[i]) {
        lv_obj_set_style_text_color(s_totales[i],
            lv_color_hex(s_oculta[i] ? 0x555555 : s_col[i]), 0);
    }
}

static void sol_close_cb(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *prev = s_prev;
    s_scr = NULL; s_prev = NULL; s_chart = NULL;
    s_ser_a = NULL; s_ser_b = NULL;
    if (prev) lv_scr_load(prev);
    if (scr) lv_obj_del(scr);
}

/* Etiquetas del eje X: 5 huecos repartidos bajo el chart. */
static void sol_set_xlabels(const char *t0, const char *t1, const char *t2,
                            const char *t3, const char *t4)
{
    if (!s_xlabels) return;
    const char *txt[5] = { t0, t1, t2, t3, t4 };
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_xlabels, i);
        if (l) lv_label_set_text(l, txt[i]);
    }
}

/* ── Modo HOY: potencia del panel de las ultimas 24 h ─────────────────────── */
static void sol_cargar_hoy(void)
{
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, SOL_CHART_PTS);
    lv_chart_set_all_value(s_chart, s_ser_a, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_b, LV_CHART_POINT_NONE);
    lv_chart_set_x_start_point(s_chart, s_ser_a, 0);

    int max_w = 0;
    bh_point_t *pts = heap_caps_malloc(sizeof(bh_point_t) * BH_POINTS, MALLOC_CAP_SPIRAM);
    if (pts) {
        int32_t t0 = 0, t1 = 0;
        const size_t n = battery_history_get_series(BH_SRC_SOLAR_CHARGER, pts, &t0, &t1);
        /* Se agrupa el historico (8640 puntos) en las columnas del chart
         * quedandose con el MAXIMO de cada grupo: asi no se pierden los picos. */
        for (int c = 0; c < SOL_CHART_PTS; c++) {
            const size_t ini = (size_t)((uint64_t)c * n / SOL_CHART_PTS);
            const size_t fin = (size_t)((uint64_t)(c + 1) * n / SOL_CHART_PTS);
            int pico = 0, pico_bat = 0;
            bool hay = false, hay_bat = false;
            for (size_t i = ini; i < fin && i < n; i++) {
                if (!pts[i].valid) continue;
                if (pts[i].pv_watts >= 0) {
                    hay = true;
                    if (pts[i].pv_watts > pico) pico = pts[i].pv_watts;
                }
                /* Potencia hacia la bateria = corriente x tension del cargador. */
                if (pts[i].centi_volts > 0 && pts[i].milli_amps > 0) {
                    const int w = (int)(((int64_t)pts[i].milli_amps *
                                         pts[i].centi_volts) / 100000);
                    hay_bat = true;
                    if (w > pico_bat) pico_bat = w;
                }
            }
            lv_chart_set_value_by_id(s_chart, s_ser_a, c,
                                     hay ? pico : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_b, c,
                                     hay_bat ? pico_bat : LV_CHART_POINT_NONE);
            if (pico > max_w) max_w = pico;
            if (pico_bat > max_w) max_w = pico_bat;
        }
        /* Horas reales en el eje X, tomadas del primer y ultimo punto. */
        if (n > 0 && t0 > 1704067200) {
            char h[5][8];
            for (int i = 0; i < 5; ++i) {
                time_t t = (time_t)(t0 + (int32_t)((int64_t)(t1 - t0) * i / 4));
                struct tm tm_l;
                localtime_r(&t, &tm_l);
                snprintf(h[i], sizeof(h[i]), "%02d:%02d", tm_l.tm_hour, tm_l.tm_min);
            }
            sol_set_xlabels(h[0], h[1], h[2], h[3], h[4]);
        } else {
            sol_set_xlabels("-24h", "-18h", "-12h", "-6h", "ahora");
        }
        heap_caps_free(pts);
    }
    /* Margen del 20 % y minimo 50 W: una noche entera no debe salir como una
     * raya pegada al borde. */
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                       (max_w > 40) ? (max_w * 12 / 10) : 50);
    lv_chart_refresh(s_chart);

    lv_label_set_text(s_lbl_rotulo, "HOY (24H)");
    lv_label_set_text(s_lbl_hint,
        "Toca un valor para mostrarlo u ocultarlo  -  el boton cambia a dias");
}

/* ── Modo POR DIAS: produccion contra consumo ─────────────────────────────── */
static void sol_cargar_dias(void)
{
    solar_day_t dias[SOL_DIAS];
    const int n = solar_daily_get_days(dias, SOL_DIAS);

    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, (n > 0) ? n : 1);
    lv_chart_set_all_value(s_chart, s_ser_a, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_b, LV_CHART_POINT_NONE);

    int tope = 0;
    for (int i = 0; i < n; i++) {
        /* En centesimas de kWh: lv_chart trabaja con enteros. */
        const int p = (int)(dias[i].kwh * 100.0f + 0.5f);
        const int c = (int)(dias[i].kwh_consumo * 100.0f + 0.5f);
        lv_chart_set_value_by_id(s_chart, s_ser_a, i, p);
        lv_chart_set_value_by_id(s_chart, s_ser_b, i, c);
        if (p > tope) tope = p;
        if (c > tope) tope = c;
    }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                       (tope > 10) ? (tope * 11 / 10) : 100);
    lv_chart_refresh(s_chart);

    if (n > 0) {
        char f[5][8];
        for (int i = 0; i < 5; ++i) {
            const int idx = (n > 1) ? ((n - 1) * i / 4) : 0;
            time_t t = (time_t)dias[idx].day_id * 86400;
            struct tm tm_l;
            localtime_r(&t, &tm_l);
            snprintf(f[i], sizeof(f[i]), "%d/%d", tm_l.tm_mday, tm_l.tm_mon + 1);
        }
        sol_set_xlabels(f[0], f[1], f[2], f[3], f[4]);
    } else {
        sol_set_xlabels("", "", "", "", "");
    }

    lv_label_set_text(s_lbl_rotulo, "POR DIAS");
    lv_label_set_text(s_lbl_hint,
        (n > 0) ? "Toca un valor para mostrarlo u ocultarlo  (kWh por dia)"
                : "Aun no hay dias completos: empieza manana");
}

static void sol_actualizar_totales(void);

static void sol_toggle_modo_cb(lv_event_t *e)
{
    (void)e;
    s_modo_dias = !s_modo_dias;
    lv_label_set_text(s_lbl_modo, s_modo_dias ? "Por dias" : "Hoy");
    if (s_modo_dias) sol_cargar_dias();
    else             sol_cargar_hoy();
    sol_actualizar_totales();
}

/* Fila de totales al pie, en el mismo formato que la de bateria. */
static void sol_actualizar_totales(void)
{
    solar_day_t hoy;
    solar_daily_get_today(&hoy);
    trip_computer_t viaje;
    trip_computer_get(&viaje);

    /* Los dos primeros son la LEYENDA (se pueden apagar); los otros dos, info. */
    if (s_totales[0]) {
        if (s_modo_dias) {
            lv_label_set_text_fmt(s_totales[0], "Producido %.2f kWh/dia",
                                  (double)solar_daily_avg(30));
        } else {
            lv_label_set_text_fmt(s_totales[0], "Panel: hoy %.2f kWh",
                                  (double)hoy.kwh);
        }
    }
    if (s_totales[1]) {
        if (s_modo_dias) {
            lv_label_set_text_fmt(s_totales[1], "Consumido %.2f kWh/dia",
                                  (double)solar_daily_avg_consumo(30));
        } else {
            lv_label_set_text(s_totales[1], "A bateria");
        }
    }
    if (s_totales[2]) {
        lv_label_set_text_fmt(s_totales[2], "Pico %d W  /  %.1f h",
                              (int)hoy.pico_w, (double)hoy.horas);
    }
    if (s_totales[3]) {
        lv_label_set_text_fmt(s_totales[3], "Viaje %.2f kWh",
                              viaje.wh_solar / 1000.0);
    }
    /* Respetar el gris de las que estan apagadas. */
    for (int i = 0; i < 2; ++i) {
        if (s_totales[i]) {
            lv_obj_set_style_text_color(s_totales[i],
                lv_color_hex(s_oculta[i] ? 0x555555 : s_col[i]), 0);
        }
    }
}

void ui_show_solar_history_screen(ui_state_t *ui)
{
    (void)ui;
    if (s_scr) return;

    lv_obj_t *prev = lv_scr_act();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_scr = scr;
    s_prev = prev;

    /* Sin beep global: su callback es privado de ui.c. No merece exponerlo solo
     * para esto. */

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, sol_close_cb, LV_EVENT_CLICKED, scr);

    /* Titulo + rotulo central */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "PLACA SOLAR");
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20_es, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 16, 16);

    s_lbl_rotulo = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_rotulo, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_rotulo, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_lbl_rotulo, "HOY (24H)");
    lv_obj_align(s_lbl_rotulo, LV_ALIGN_TOP_MID, 0, 16);

    /* Boton de modo (mismo sitio y tamano que el de Corriente/Tension) */
    lv_obj_t *bmode = lv_btn_create(scr);
    lv_obj_set_size(bmode, 140, 40);
    lv_obj_set_style_bg_color(bmode, lv_color_hex(0x2A3340), 0);
    lv_obj_set_style_radius(bmode, 8, 0);
    lv_obj_align(bmode, LV_ALIGN_TOP_LEFT, 16, 84);
    s_lbl_modo = lv_label_create(bmode);
    lv_label_set_text(s_lbl_modo, "Hoy");
    lv_obj_set_style_text_font(s_lbl_modo, &lv_font_montserrat_20_es, 0);
    lv_obj_center(s_lbl_modo);
    lv_obj_add_event_cb(bmode, sol_toggle_modo_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0x8A93A6), 0);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_14_es, 0);
    lv_label_set_text(s_lbl_hint, "Potencia del panel  -  el boton cambia a dias");
    lv_obj_align(s_lbl_hint, LV_ALIGN_TOP_MID, 0, 94);

    /* Chart: mismo tamano y posicion que el del historico de bateria */
    lv_obj_t *chart = lv_chart_create(scr);
    lv_obj_set_size(chart, LV_HOR_RES - 150, LV_VER_RES - 210);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(chart, 5, 8);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* sin puntos gordos */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 8, 4, 5, 1, true, 80);
    lv_obj_set_style_pad_left(chart, 8, 0);
    lv_obj_set_style_pad_top(chart, 16, 0);
    lv_obj_set_style_text_color(chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(chart, &lv_font_montserrat_20_es, LV_PART_TICKS);
    s_chart = chart;

    /* Series: ambar = produccion, azul = consumo (solo se usa en POR DIAS).
     * Se crean antes de fijar point_count, como en el historico de bateria:
     * al reves LVGL redimensiona con cada add_series y se cuelga. */
    s_ser_a = lv_chart_add_series(chart, lv_color_hex(0xFFD54F), LV_CHART_AXIS_PRIMARY_Y);
    vTaskDelay(1);
    s_ser_b = lv_chart_add_series(chart, lv_color_hex(0x4FC3F7), LV_CHART_AXIS_PRIMARY_Y);
    vTaskDelay(1);

    /* Etiquetas del eje X bajo el chart */
    s_xlabels = lv_obj_create(scr);
    lv_obj_remove_style_all(s_xlabels);
    lv_obj_set_size(s_xlabels, LV_HOR_RES - 150, 28);
    lv_obj_align_to(s_xlabels, chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_layout(s_xlabels, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_xlabels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_xlabels, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_label_create(s_xlabels);
        lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_text(l, "--:--");
    }

    /* Fila de totales al pie (misma maqueta que la de bateria) */
    lv_obj_t *totals = lv_obj_create(scr);
    lv_obj_remove_style_all(totals);
    lv_obj_set_size(totals, LV_HOR_RES - 32, 30);
    lv_obj_align(totals, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    lv_obj_set_layout(totals, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(totals, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(totals, 8, 0);
    static const uint32_t col[4] = { 0xFFD54F, 0x4FC3F7, 0xAED581, 0xFF8A65 };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = lv_label_create(totals);
        lv_obj_set_width(l, (LV_HOR_RES - 32 - 24) / 4);
        lv_obj_set_style_text_color(l, lv_color_hex(col[i]), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_label_set_text(l, "--");
        /* Las dos primeras son la leyenda: tocarlas apaga o enciende su serie. */
        if (i < 2) {
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(l, sol_legend_toggle_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
        s_totales[i] = l;
    }
    sol_actualizar_totales();

    s_modo_dias = false;
    sol_cargar_hoy();

    lv_scr_load(scr);
}
