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

/* Definida en ui.c (usada tambien en el nucleo de la UI) */
extern void ui_global_click_beep_cb(lv_event_t *e);

/* Pulso on/off de altura fija del OrionTR: igual que en ui.c (main.c/ui.c,
 * usada para el mismo calculo en HOY). */
#define ORION_TR_ON_MILLIAMPS  10000   /* 10 A */

static const char *TAG_UI = "UI_MODULE";

/* --- Pantalla historico bateria --- */
static lv_obj_t *s_bh_screen = NULL;
static lv_obj_t *s_bh_lbl_date = NULL;
static lv_obj_t *s_bh_xlabels  = NULL;
static lv_obj_t *s_bh_lbl_zoom = NULL;
static lv_obj_t *s_bh_totals[BH_SRC_COUNT] = {NULL};
static lv_chart_series_t *s_bh_series[BH_SRC_COUNT] = {NULL};
/* Leyenda-boton: tocar el total de una fuente (BM/Solar/Orion/AC) muestra/oculta
 * su serie. Guardamos su color para restaurar la etiqueta al mostrarla. */
static bool     s_bh_hidden[BH_SRC_COUNT] = {false};
static uint32_t s_bh_col[BH_SRC_COUNT]    = {0};
/* Nombres cortos para la fila de totales (cabe en una sola linea). El nombre
 * completo (battery_history_source_name) se sigue usando en el CSV. */
static const char *s_bh_short_names[BH_SRC_COUNT] = {
    "Bateria", "Solar", "OrionTR", "AC",
};
static int  s_bh_day_idx = -1;
static int  s_bh_n_dates = 0;
static char s_bh_dates[LOG_BROWSER_MAX_DATES][LOG_BROWSER_DATE_LEN];
/* Ventana de visualizacion sobre los datos del dia (fraccion 0..1). */
static float s_bh_win_a = 0.0f;
static float s_bh_win_b = 1.0f;
/* Modo de la grafica: false = corriente (4 fuentes), true = tension BM (12-14V).
 * Los valores del chart se guardan en decimas de unidad (deci-A / deci-V) para
 * conservar 1 decimal; bh_y_tick_draw_cb formatea el eje dividiendo por 10. */
static bool s_bh_show_voltage = false;
static lv_obj_t *s_bh_lbl_mode = NULL;

/* Estado del gesto tactil (arrastrar = pan, doble-toque = zoom, manten = 1x) */
static int32_t s_bh_drag_last_x   = 0;
static int32_t s_bh_drag_moved    = 0;
static bool    s_bh_dragging      = false;
static int64_t s_bh_last_apply_us = 0;
static int64_t s_bh_last_click_us = 0;

#define BH_LOG_MAX_ENTRIES   8800   /* 24h completas @10s (8640) + margen */
/* Un buffer por fuente, en PSRAM: 4 x 8800 x 16 B ~= 563 KB, imposible en RAM
 * interna estatica. Se alojan una vez (lazy) al consultar un dia historico. */
static battery_log_entry_t *s_bh_buf[BH_SRC_COUNT] = {NULL};
/* Cache: dia (idx) ya parseado en s_bh_buf y cuantas entradas tiene cada fuente
 * (mismo motivo que el frigo: no re-leer el CSV de la SD en cada tick de
 * pan/zoom). -2 = cache vacia O carga en curso; ver bh_loader_task. */
static int s_bh_loaded_idx = -2;
static int s_bh_loaded_n[BH_SRC_COUNT] = {0};
/* Totales del dia completo (no de la ventana visible), calculados una vez al
 * cargar: Ah cargados / descargados por fuente. */
static float s_bh_tot_ch[BH_SRC_COUNT]  = {0};
static float s_bh_tot_dis[BH_SRC_COUNT] = {0};

/* Carga de un dia historico: la hace una tarea aparte, NO el callback de LVGL.
 *
 * Leer y parsear el CSV de un dia tarda segundos, y bh_chart_load_day corre
 * dentro de un callback de LVGL, o sea con el cerrojo de LVGL tomado. Si ese
 * cerrojo se retiene mas de ~9 s, el watchdog SW (main/watchdog.c: 3 fallos
 * consecutivos x 3 s) da la UI por congelada y REINICIA LA PLACA. Eso es lo que
 * pasaba al navegar por dias, y trocear la lectura no lo arreglaba: el
 * watchdog mira tiempo de reloj, no CPU, y los yields lo alargaban.
 *
 * Reparto: la tarea lee de la SD sin ningun cerrojo de LVGL y solo lo toma al
 * final, unos ms, para pintar. Los buffers los escribe unicamente la tarea, y
 * solo mientras s_bh_loaded_idx == -2; como todo el que los lee lo hace con el
 * cerrojo de LVGL tomado y comprobando ese indice, no hay carrera. */
static TaskHandle_t   s_bh_loader_task = NULL;
static volatile int   s_bh_req_idx     = -2;   /* dia pedido a la tarea */

static void bh_chart_load_day(void);
static void bh_paint_hist_day(void);
static void bh_loader_task(void *arg);
static void bh_chart_gesture_cb(lv_event_t *e);
static void bh_arrow_cb(lv_event_t *e);
static void bh_chart_touch_cb(lv_event_t *e);
static void bh_apply_window(void);
static void bh_update_zoom_label(void);

static lv_obj_t *s_bh_chart  = NULL;

/* Rango Y actual del chart (deci-A en corriente / deci-V en tension). Lo
 * guardamos al fijar el rango para poder dibujar la linea del 0 en pixeles. */
static int32_t s_bh_y_min = 0;
static int32_t s_bh_y_max = 0;

static lv_obj_t *s_bh_prev_screen = NULL;

void ui_close_battery_history_screen(void)
{
    if (s_bh_screen) {
        lv_obj_t *prev = s_bh_prev_screen;
        if (!prev) prev = lv_disp_get_scr_prev(NULL);
        /* auto_del=true → LVGL borra s_bh_screen al terminar la transición.
         * Hacerlo manualmente con lv_obj_del aquí provoca corrupción porque
         * la pantalla aún puede estar siendo renderizada. */
        if (prev) lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
        s_bh_screen = NULL;
        s_bh_chart = NULL;
        s_bh_prev_screen = NULL;
    }
    /* Los buffers de carga (~800 KB de PSRAM entre las 4 fuentes y el volcado
     * del anillo) NO se liberan aqui a proposito: bh_loader_task puede estar
     * escribiendo en ellos justo ahora, y esta funcion corre en el hilo de
     * LVGL, que no la espera -> liberarlos seria un use-after-free. Se
     * reservan una sola vez y se reutilizan en cada visita; sobra PSRAM (32 MB)
     * y asi tampoco se refragmenta. Lo que si se invalida es la cache. */
    s_bh_loaded_idx = -2;
}

static void bh_screen_close_cb(lv_event_t *e)
{
    (void)e;
    ui_close_battery_history_screen();
}

/* Toca el total de una fuente -> muestra/oculta su serie y atenua la etiqueta. */
static void bh_legend_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= BH_SRC_COUNT || !s_bh_chart || !s_bh_series[i]) return;
    bool hide = !s_bh_hidden[i];
    s_bh_hidden[i] = hide;
    lv_chart_hide_series(s_bh_chart, s_bh_series[i], hide);
    if (s_bh_totals[i])
        lv_obj_set_style_text_color(s_bh_totals[i],
            hide ? lv_color_hex(0x555555) : lv_color_hex(s_bh_col[i]), 0);
}

/* Formatea las etiquetas del eje Y con 1 decimal: los valores del chart se
 * guardan en decimas (deci-A en modo corriente, deci-V en modo tension), asi
 * que dividimos por 10. Ej: 132 -> "13.2", -5 -> "-0.5". */
static void bh_y_tick_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class,
                                     LV_CHART_DRAW_PART_TICK_LABEL))
        return;
    if (dsc->id != LV_CHART_AXIS_PRIMARY_Y || !dsc->text) return;
    int32_t v = dsc->value;
    int32_t whole = v / 10;
    int32_t frac = v % 10;
    if (frac < 0) frac = -frac;
    const char *sign = (v < 0 && whole == 0) ? "-" : "";
    lv_snprintf(dsc->text, dsc->text_length, "%s%ld.%ld",
                sign, (long)whole, (long)frac);
}

/* Dibuja una raya fina en el valor 0 (solo en modo Corriente) para ver de un
 * vistazo si esta cargando (por encima) o descargando (por debajo). En modo
 * Tension no aplica y no se dibuja. */
static void bh_zero_line_draw_cb(lv_event_t *e)
{
    if (s_bh_show_voltage) return;
    int32_t range = s_bh_y_max - s_bh_y_min;
    if (range <= 0) return;
    if (s_bh_y_min > 0 || s_bh_y_max < 0) return;  /* el 0 cae fuera del rango */

    lv_obj_t *chart = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_content_coords(chart, &a);
    lv_coord_t h = a.y2 - a.y1;
    lv_coord_t y0 = a.y2 - (lv_coord_t)((int64_t)(0 - s_bh_y_min) * h / range);

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x9AA0A6);
    line_dsc.width = 2;
    line_dsc.opa = LV_OPA_COVER;
    lv_point_t p1 = { a.x1, y0 };
    lv_point_t p2 = { a.x2, y0 };
    lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
}

/* Alterna la grafica entre modo corriente (4 fuentes) y tension (BM, 12-14V). */
static void bh_toggle_mode_cb(lv_event_t *e)
{
    (void)e;
    s_bh_show_voltage = !s_bh_show_voltage;
    if (s_bh_lbl_mode)
        lv_label_set_text(s_bh_lbl_mode,
                          s_bh_show_voltage ? "Tension" : "Corriente");
    bh_chart_load_day();
}

void ui_show_battery_history_screen(ui_state_t *ui)
{
    (void)ui;
    if (s_bh_screen) return;

    lv_obj_t *prev = lv_scr_act();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_bh_screen = scr;
    s_bh_prev_screen = prev;

    /* Beep al pulsar cualquier widget tambien en esta pantalla aparte */
    lv_obj_add_event_cb(scr, ui_global_click_beep_cb, LV_EVENT_CLICKED, NULL);

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    /* Pasamos el screen al cb para borrarlo y volver al anterior */
    lv_obj_add_event_cb(btn_close, bh_screen_close_cb, LV_EVENT_CLICKED, scr);
    /* truco: en el cb usamos user_data == scr y lv_obj_get_screen(scr) devuelve scr,
       asi que cargamos la pantalla anterior por su puntero capturado */

    /* Titulo (izquierda) + fecha (centro) + flechas indicando swipe */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "HISTORICO BATERIA");
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20_es, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 16, 16);

    s_bh_lbl_date = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bh_lbl_date, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_bh_lbl_date, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_bh_lbl_date, "HOY (24H)");
    lv_obj_align(s_bh_lbl_date, LV_ALIGN_TOP_MID, 0, 16);

    /* Flechas PULSABLES. Antes eran etiquetas decorativas "para indicar swipe":
     * parecian controles y no hacian nada al tocarlas. Area de toque ampliada
     * porque el glifo solo es diminuto para un dedo. */
    lv_obj_t *bh_arr_l = lv_label_create(scr);
    lv_obj_set_style_text_font(bh_arr_l, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bh_arr_l, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(bh_arr_l, LV_SYMBOL_LEFT);
    lv_obj_align(bh_arr_l, LV_ALIGN_TOP_MID, -150, 18);
    lv_obj_add_flag(bh_arr_l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bh_arr_l, 30);
    lv_obj_add_event_cb(bh_arr_l, bh_arrow_cb, LV_EVENT_CLICKED, (void *)1);  /* atras */
    lv_obj_t *bh_arr_r = lv_label_create(scr);
    lv_obj_set_style_text_font(bh_arr_r, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bh_arr_r, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(bh_arr_r, LV_SYMBOL_RIGHT);
    lv_obj_align(bh_arr_r, LV_ALIGN_TOP_MID, 150, 18);
    lv_obj_add_flag(bh_arr_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bh_arr_r, 30);
    lv_obj_add_event_cb(bh_arr_r, bh_arrow_cb, LV_EVENT_CLICKED, NULL);       /* adelante */

    /* Totales: una sola fila con las 4 fuentes (nombres cortos para que
     * quepan en horizontal sin envolver). */
    lv_obj_t *totals_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(totals_cont);
    lv_obj_set_size(totals_cont, LV_HOR_RES - 32, 30);
    lv_obj_align(totals_cont, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    lv_obj_set_layout(totals_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(totals_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(totals_cont, 8, 0);

    static const uint32_t colors[BH_SRC_COUNT] = {
        0x4FC3F7, /* BM cyan */
        0xFFD54F, /* Solar amber */
        0xFF8A65, /* Orion orange */
        0xAED581, /* AC green */
    };
    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        lv_obj_t *l = lv_label_create(totals_cont);
        lv_obj_set_width(l, (LV_HOR_RES - 32 - 24) / 4);
        lv_obj_set_style_text_color(l, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(l, "%s +0.0/-0.0 Ah", s_bh_short_names[i]);
        /* La etiqueta de total es tambien el boton de leyenda: tocar = mostrar/
         * ocultar esa fuente en la grafica. */
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, bh_legend_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_bh_col[i]    = colors[i];
        s_bh_hidden[i] = false;
        s_bh_totals[i] = l;
    }

    s_bh_win_a = 0.0f;
    s_bh_win_b = 1.0f;

    /* Boton de modo: alterna Corriente / Tension (izquierda) */
    {
        lv_obj_t *bmode = lv_btn_create(scr);
        lv_obj_set_size(bmode, 140, 40);
        lv_obj_set_style_bg_color(bmode, lv_color_hex(0x2A3340), 0);
        lv_obj_set_style_radius(bmode, 8, 0);
        lv_obj_align(bmode, LV_ALIGN_TOP_LEFT, 16, 84);
        s_bh_lbl_mode = lv_label_create(bmode);
        lv_label_set_text(s_bh_lbl_mode,
                          s_bh_show_voltage ? "Tension" : "Corriente");
        lv_obj_set_style_text_font(s_bh_lbl_mode, &lv_font_montserrat_20_es, 0);
        lv_obj_center(s_bh_lbl_mode);
        lv_obj_add_event_cb(bmode, bh_toggle_mode_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Pista de uso tactil (zoom/pan sin botones) */
    {
        lv_obj_t *hint = lv_label_create(scr);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8A93A6), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14_es, 0);
        lv_label_set_text(hint,
            "Arrastra: mover  -  2 toques: zoom  -  manten: 1x");
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 94);
    }

    /* Etiqueta de nivel de zoom (derecha) */
    s_bh_lbl_zoom = lv_label_create(scr);
    lv_obj_set_style_text_color(s_bh_lbl_zoom, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_bh_lbl_zoom, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(s_bh_lbl_zoom, "1x");
    lv_obj_align(s_bh_lbl_zoom, LV_ALIGN_TOP_RIGHT, -16, 92);

    lv_obj_t *chart = lv_chart_create(scr);
    /* Estrechamos el chart para dejar ~75 px libres a cada lado: LVGL dibuja
     * las etiquetas del eje Y a la IZQUIERDA del borde del chart (x_ofs =
     * coords.x1), asi que si el chart llega al borde de pantalla las etiquetas
     * se salen. Con el chart centrado y mas estrecho, caben en pantalla. */
    lv_obj_set_size(chart, LV_HOR_RES - 150, LV_VER_RES - 210);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(chart, bh_chart_touch_cb, LV_EVENT_ALL, NULL);
    /* Sin esto el gesto de deslizar MUERE en la grafica: LVGL lo entrega al
     * objeto tocado y solo sube al padre con EVENT_BUBBLE. Como la grafica ocupa
     * casi toda la pantalla, deslizar donde es natural no cambiaba de dia. */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(chart, 5, 8);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    /* Limitar puntos del chart LVGL a un nº manejable; con BH_POINTS=8640
     * y 4 series LVGL aloca demasiado y el render se cuelga. Hacemos
     * downsample por step antes de meter los puntos.
     * 300 puntos x 4 series = 1200 valores, ~5 min de resolucion en 24h.
     * 1500 cuelga taskLVGL en set_point_count tras add_series (WDT). */
    const int CHART_MAX_PTS = 300;
    int chart_pts = (BH_POINTS > CHART_MAX_PTS) ? CHART_MAX_PTS : BH_POINTS;
    int chart_step = (BH_POINTS + chart_pts - 1) / chart_pts;
    if (chart_step < 1) chart_step = 1;
    chart_pts = (BH_POINTS + chart_step - 1) / chart_step;
    /* CRITICO: crear las series PRIMERO con point_count default (pequeno).
     * Con point_count=1440 antes, la 3a llamada a lv_chart_add_series
     * cuelga taskLVGL > 5s (WDT). El set_point_count(1440) final
     * redimensiona las 4 a la vez de forma controlada. */
    /* Hueco Y holgado: en corriente las etiquetas llegan a "-120.0" (6 chars)
     * a fuente 20 y se recortaba el signo con menos espacio. */
    /* draw_size 80: permite dibujar las etiquetas Y fuera (a la izquierda) del
     * area del chart sin que se recorten. */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 8, 4, 5, 1, true, 80);
    lv_obj_set_style_pad_left(chart, 8, 0);
    /* Hueco arriba para que la etiqueta Y superior (fuente 20) no se recorte. */
    lv_obj_set_style_pad_top(chart, 16, 0);
    lv_obj_set_style_text_color(chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(chart, &lv_font_montserrat_20_es, LV_PART_TICKS);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    /* Etiquetas del eje Y con 1 decimal (deci-A / deci-V) */
    lv_obj_add_event_cb(chart, bh_y_tick_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    /* Raya del 0 (carga/descarga) por encima de la rejilla y las series. */
    lv_obj_add_event_cb(chart, bh_zero_line_draw_cb, LV_EVENT_DRAW_POST_END, NULL);
    s_bh_chart = chart;
    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        s_bh_series[i] = lv_chart_add_series(chart,
            lv_color_hex(colors[i]), LV_CHART_AXIS_PRIMARY_Y);
        vTaskDelay(1);  /* yield para que IDLE corra entre series */
    }
    lv_chart_set_point_count(chart, chart_pts);

    /* Labels horarios bajo el chart (5 huecos, refrescados al cambiar de dia) */
    s_bh_xlabels = lv_obj_create(scr);
    lv_obj_remove_style_all(s_bh_xlabels);
    lv_obj_set_size(s_bh_xlabels, LV_HOR_RES - 150, 28);
    lv_obj_align_to(s_bh_xlabels, chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_layout(s_bh_xlabels, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_bh_xlabels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bh_xlabels, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_label_create(s_bh_xlabels);
        lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_text(l, "--:--");
    }

    /* Listar fechas SD y arrancar en HOY */
    s_bh_n_dates = log_browser_list_dates("/sdcard/bateria",
                                          s_bh_dates, LOG_BROWSER_MAX_DATES);
    if (s_bh_n_dates > 0) {
        char today[11];
        today_ymd(today);
        if (strcmp(s_bh_dates[s_bh_n_dates - 1], today) == 0)
            s_bh_n_dates--;
    }
    s_bh_day_idx = -1;
    s_bh_loaded_idx = -2;   /* al abrir la pantalla, releer (ver bh_step_day) */
    /* Lector de dias historicos: se crea una vez y vive lo que la aplicacion
     * (los buffers en PSRAM tambien se reutilizan entre visitas). Prioridad 3,
     * por debajo de la de LVGL, para no robarle tiempo a la UI. */
    if (!s_bh_loader_task) {
        if (xTaskCreate(bh_loader_task, "bh_loader", 5120, NULL, 3,
                        &s_bh_loader_task) != pdPASS) {
            s_bh_loader_task = NULL;
            ESP_LOGE(TAG_UI, "no se pudo crear bh_loader: los dias historicos no cargaran");
        }
    }
    bh_chart_load_day();

    /* Gestures: swipe izq/dcha */
    lv_obj_add_event_cb(scr, bh_chart_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_bh_prev_screen = prev;
    lv_scr_load(scr);
}

static void bh_clear_chart_series(void)
{
    if (!s_bh_chart) return;
    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        if (!s_bh_series[i]) continue;
        lv_chart_set_all_value(s_bh_chart, s_bh_series[i], LV_CHART_POINT_NONE);
        lv_chart_set_x_start_point(s_bh_chart, s_bh_series[i], 0);
    }
}

/* Etiquetas horarias de un dia historico. `src` es la fuente de la que se sacan
 * las horas (la que mas muestras tenga: todas comparten el mismo eje temporal,
 * pero una puede estar vacia ese dia). */
static void bh_update_xlabels_from_buf(int src, int n)
{
    if (!s_bh_xlabels) return;
    if (n <= 0 || src < 0 || !s_bh_buf[src]) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_t *l = lv_obj_get_child(s_bh_xlabels, i);
            if (l) lv_label_set_text(l, "--:--");
        }
        return;
    }
    int a = (int)(s_bh_win_a * n);
    int b = (int)(s_bh_win_b * n);
    if (b <= a) b = a + 1;
    if (b > n) b = n;
    int wn = b - a;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_bh_xlabels, i);
        if (!l) continue;
        int idx = a + (wn - 1) * i / 4;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        lv_label_set_text_fmt(l, "%02d:%02d",
                              s_bh_buf[src][idx].hh, s_bh_buf[src][idx].mm);
    }
}

/* Punto de entrada unico para pintar el dia en curso (s_bh_day_idx): -1 = HOY,
 * >=0 = indice en s_bh_dates. Si ese dia ya esta en los buffers se repinta y
 * ya; si no, se le pide a bh_loader_task y se deja el aviso puesto. Nunca toca
 * la SD: corre dentro de callbacks de LVGL. */
static void bh_chart_load_day(void)
{
    if (!s_bh_chart) return;

    if (s_bh_day_idx == s_bh_loaded_idx) {
        bh_paint_hist_day();      /* el dia ya esta en RAM: solo repintar */
    } else {
        /* Dia todavia sin leer: se lo pide a bh_loader_task y deja la grafica
         * vacia con un aviso. Aqui NO se toca la SD: estamos dentro de un
         * callback de LVGL y leer el CSV congelaria la UI hasta el reinicio
         * (ver el comentario de s_bh_loader_task). */
        bh_clear_chart_series();
        if (s_bh_lbl_date) {
            if (s_bh_day_idx < 0) {
                lv_label_set_text(s_bh_lbl_date, "HOY (24H) ...");
            } else {
                char disp[11];
                fmt_date_ddmmaaaa(s_bh_dates[s_bh_day_idx], disp, sizeof disp);
                lv_label_set_text_fmt(s_bh_lbl_date, "%s ...", disp);
            }
        }
        for (int s = 0; s < BH_SRC_COUNT; ++s) {
            if (s_bh_totals[s])
                lv_label_set_text_fmt(s_bh_totals[s], "%s ...", s_bh_short_names[s]);
        }
        s_bh_req_idx = s_bh_day_idx;
        if (s_bh_loader_task) xTaskNotifyGive(s_bh_loader_task);
    }
    lv_chart_refresh(s_bh_chart);
}

/* Pinta el dia historico que ya esta en s_bh_buf. Solo lectura de los buffers:
 * hay que llamarla con el cerrojo de LVGL tomado (desde un callback, o desde
 * bh_loader_task tras cogerlo) y con s_bh_loaded_idx == s_bh_day_idx. */
static void bh_paint_hist_day(void)
{
    if (!s_bh_chart) return;
    bh_clear_chart_series();

    /* Fuente con mas muestras: manda en el eje X y en el numero de puntos. */
    int ref = 0, n_ref = 0;
    for (int s = 0; s < BH_SRC_COUNT; ++s) {
        if (s_bh_loaded_n[s] > n_ref) { n_ref = s_bh_loaded_n[s]; ref = s; }
    }

    const int CHART_MAX_PTS = 300;  /* mismo limite seguro que la rama HOY (WDT) */
    int wa_ref = (int)(s_bh_win_a * n_ref);
    int wb_ref = (int)(s_bh_win_b * n_ref);
    if (wb_ref <= wa_ref) wb_ref = wa_ref + 1;
    if (wb_ref > n_ref) wb_ref = n_ref;
    int wn = wb_ref - wa_ref;
    int pts_cnt = wn > 0 ? wn : 2;
    if (pts_cnt > CHART_MAX_PTS) pts_cnt = CHART_MAX_PTS;
    if (pts_cnt < 2) pts_cnt = 2;
    lv_chart_set_point_count(s_bh_chart, pts_cnt);

    int32_t bmin = INT32_MAX, bmax = INT32_MIN;
    for (int s = 0; s < BH_SRC_COUNT; ++s) {
        /* En modo tension solo se grafica el BatteryMonitor, igual que en HOY. */
        if (s_bh_show_voltage && s != BH_SRC_BATTERY_MONITOR) continue;
        int n = s_bh_loaded_n[s];
        if (n <= 0 || !s_bh_buf[s]) continue;
        /* Ventana per-source sobre SUS muestras: una fuente que arranco tarde
         * tiene menos puntos y con los indices de `ref` se saldria de rango. */
        int wa = (int)(s_bh_win_a * n);
        int wb = (int)(s_bh_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > n) wb = n;
        int step = ((wb - wa) > CHART_MAX_PTS)
                 ? ((wb - wa) + CHART_MAX_PTS - 1) / CHART_MAX_PTS : 1;
        int idx = 0;
        for (int i = wa; i < wb && idx < pts_cnt; i += step) {
            int32_t a;
            if (s_bh_show_voltage) {
                int32_t cv = s_bh_buf[s][i].centi_volts;
                if (cv <= 0) {   /* CSV viejo sin columna de tension */
                    lv_chart_set_value_by_id(s_bh_chart, s_bh_series[s], idx,
                                             LV_CHART_POINT_NONE);
                    idx++;
                    continue;
                }
                a = cv / 10;                        /* centi-V -> deci-V */
            } else {
                a = s_bh_buf[s][i].milli_amps / 100; /* milli-A -> deci-A */
            }
            if (a < bmin) bmin = a;
            if (a > bmax) bmax = a;
            lv_chart_set_value_by_id(s_bh_chart, s_bh_series[s], idx, (lv_coord_t)a);
            idx++;
        }
    }
    if (bmin == INT32_MAX) {
        if (s_bh_show_voltage) { bmin = 120; bmax = 140; }  /* 12.0-14.0 V */
        else                   { bmin = -40; bmax = 40; }
    }
    int32_t span = bmax - bmin; if (span < 1) span = 1;
    s_bh_y_min = bmin - span / 20 - 1;
    s_bh_y_max = bmax + span / 20 + 1;
    lv_chart_set_range(s_bh_chart, LV_CHART_AXIS_PRIMARY_Y,
                       s_bh_y_min, s_bh_y_max);
    bh_update_xlabels_from_buf(ref, n_ref);

    for (int s = 0; s < BH_SRC_COUNT; ++s) {
        if (!s_bh_totals[s]) continue;
        if (s_bh_loaded_n[s] == 0) {
            lv_label_set_text_fmt(s_bh_totals[s], "%s (s/d)", s_bh_short_names[s]);
        } else if (s == BH_SRC_ORION_XS) {
            /* Igual que en HOY: el OrionTR es un pulso on/off de altura fija,
             * su "Ah" no es real; se ensena como horas de carga. */
            float hours = s_bh_tot_ch[s] / (ORION_TR_ON_MILLIAMPS / 1000.0f);
            lv_label_set_text_fmt(s_bh_totals[s], "%s %.1f h",
                                  s_bh_short_names[s], hours);
        } else {
            lv_label_set_text_fmt(s_bh_totals[s], "%s +%.1f/-%.1f Ah",
                                  s_bh_short_names[s],
                                  s_bh_tot_ch[s], s_bh_tot_dis[s]);
        }
    }
    if (s_bh_lbl_date) {
        if (s_bh_loaded_idx < 0) {
            lv_label_set_text(s_bh_lbl_date, "HOY (24H)");
        } else {
            char disp[11];
            fmt_date_ddmmaaaa(s_bh_dates[s_bh_loaded_idx], disp, sizeof disp);
            lv_label_set_text(s_bh_lbl_date, disp);
        }
    }
    lv_chart_refresh(s_bh_chart);
}

/* Pega al final de s_bh_buf las muestras del anillo en PSRAM posteriores a la
 * ultima que ya venia del CSV, y actualiza n[]. Solo para HOY.
 *
 * Las dos mitades son necesarias: el CSV llega hasta el ultimo volcado (cada
 * 60 s) pero SOBREVIVE a los reinicios, y el anillo tiene el minuto en curso
 * pero arranca vacio en cada arranque. Juntos dan el dia entero.
 *
 * Corre en bh_loader_task: sin cerrojo de LVGL y con la cache ya invalidada. */
static void bh_append_ring_tail(int *n)
{
    /* Sin RTC en hora, battery_history guarda uptime en `ts` (y el CSV escribe
     * "BOOT+n", que no parsea): mismo umbral que usa el componente. */
    const int32_t TS_EPOCH_MIN = 1704067200;   /* 2024-01-01 */

    static bh_point_t *ring = NULL;   /* 8640 x 28 B ~= 242 KB; se reutiliza */
    if (!ring) {
        ring = heap_caps_malloc(sizeof(bh_point_t) * BH_POINTS, MALLOC_CAP_SPIRAM);
        if (!ring) { ESP_LOGE(TAG_UI, "sin PSRAM para leer el anillo"); return; }
    }

    for (int s = 0; s < BH_SRC_COUNT; ++s) {
        if (!s_bh_buf[s]) continue;
        int32_t ots = 0, nts = 0;
        int cnt = (int)battery_history_get_series((bh_source_t)s, ring, &ots, &nts);
        /* Minuto de la ultima muestra que ya trae el CSV: se anade solo lo
         * posterior para no duplicar el solape. -1 = el CSV no tenia nada. */
        int last_min = (n[s] > 0)
            ? s_bh_buf[s][n[s] - 1].hh * 60 + s_bh_buf[s][n[s] - 1].mm
            : -1;
        for (int i = 0; i < cnt && n[s] < BH_LOG_MAX_ENTRIES; ++i) {
            if (!ring[i].valid) continue;
            int hh = 0, mm = 0, minute = -1;
            if (ring[i].ts > TS_EPOCH_MIN) {
                time_t t = ring[i].ts;
                struct tm tm_p;
                localtime_r(&t, &tm_p);
                hh = tm_p.tm_hour;
                mm = tm_p.tm_min;
                minute = hh * 60 + mm;
                if (minute <= last_min) continue;   /* ya venia en el CSV */
            }
            battery_log_entry_t *e = &s_bh_buf[s][n[s]];
            e->hh = hh;
            e->mm = mm;
            e->milli_amps  = ring[i].milli_amps;
            e->centi_volts = ring[i].centi_volts;
            n[s]++;
        }
    }
}

/* Lee de la SD el dia que pida s_bh_req_idx y lo pinta. Ver el comentario de
 * s_bh_loader_task para el reparto de cerrojos. */
static void bh_loader_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int idx = s_bh_req_idx;
        while (idx >= -1 && idx < s_bh_n_dates && idx != s_bh_loaded_idx) {
            for (int s = 0; s < BH_SRC_COUNT; ++s) {
                if (s_bh_buf[s]) continue;
                s_bh_buf[s] = heap_caps_malloc(
                    sizeof(battery_log_entry_t) * BH_LOG_MAX_ENTRIES,
                    MALLOC_CAP_SPIRAM);
                if (!s_bh_buf[s]) ESP_LOGE(TAG_UI, "sin PSRAM para la fuente %d", s);
            }
            /* Invalidar la cache ANTES de escribir los buffers, y hacerlo con el
             * cerrojo tomado: asi nadie puede estar pintando de ellos mientras
             * se sobrescriben. */
            if (lvgl_port_lock(1000)) {
                s_bh_loaded_idx = -2;
                lvgl_port_unlock();
            }

            /* HOY tambien sale de la tarjeta: su CSV es lo UNICO que sobrevive a
             * un reinicio (el anillo en PSRAM arranca vacio). Lo del anillo se
             * le pega despues, en bh_append_ring_tail. */
            char date[LOG_BROWSER_DATE_LEN];
            if (idx < 0) {
                time_t now = time(NULL);
                struct tm tm_now;
                localtime_r(&now, &tm_now);
                strftime(date, sizeof(date), "%Y-%m-%d", &tm_now);
            } else {
                snprintf(date, sizeof(date), "%s", s_bh_dates[idx]);
            }
            char path[64];
            snprintf(path, sizeof(path), "/sdcard/bateria/%s.csv", date);
            int n[BH_SRC_COUNT] = {0};
            int64_t t0 = esp_timer_get_time();
            log_browser_load_battery(path, s_bh_buf, BH_LOG_MAX_ENTRIES, n);
            int from_sd = n[0] + n[1] + n[2] + n[3];
            if (idx < 0) bh_append_ring_tail(n);
            ESP_LOGI(TAG_UI, "%s cargado en %lld ms: %d de la SD + %d del anillo "
                     "(BM=%d solar=%d orion=%d ac=%d)",
                     idx < 0 ? "HOY" : date, (esp_timer_get_time() - t0) / 1000,
                     from_sd, n[0] + n[1] + n[2] + n[3] - from_sd,
                     n[0], n[1], n[2], n[3]);

            if (s_bh_req_idx != idx) { idx = s_bh_req_idx; continue; }  /* cambio de dia a mitad */

            /* Totales del dia completo. Se calculan aqui, fuera del cerrojo:
             * son hasta 4 x 8640 sumas. Sample medio de 10 s (BH_POINTS). */
            for (int s = 0; s < BH_SRC_COUNT; ++s) {
                int64_t ch = 0, dis = 0;
                for (int i = 0; i < n[s]; i++) {
                    int32_t ma = s_bh_buf[s][i].milli_amps;
                    if (ma > 0) ch += ma; else dis += -ma;
                }
                s_bh_tot_ch[s]  = (float)(ch  * 10) / (1000.0f * 3600.0f);
                s_bh_tot_dis[s] = (float)(dis * 10) / (1000.0f * 3600.0f);
            }

            if (lvgl_port_lock(1000)) {
                for (int s = 0; s < BH_SRC_COUNT; ++s) s_bh_loaded_n[s] = n[s];
                s_bh_loaded_idx = idx;
                /* Puede haberse cerrado la pantalla o cambiado de dia mientras
                 * se leia: solo pintamos si sigue siendo el dia en curso. */
                if (s_bh_chart && s_bh_day_idx == idx) bh_paint_hist_day();
                lvgl_port_unlock();
            }
            idx = s_bh_req_idx;
        }
    }
}

/* Un dia adelante o atras. `atras` = hacia el pasado. Lo comparten el gesto y
 * los botones de flecha para que no puedan divergir. */
static void bh_step_day(bool atras)
{
    if (atras) {
        if (s_bh_day_idx == -1) {
            if (s_bh_n_dates > 0) s_bh_day_idx = s_bh_n_dates - 1;
            else return;
        } else if (s_bh_day_idx > 0) {
            s_bh_day_idx--;
        } else {
            return;
        }
    } else {
        if (s_bh_day_idx < 0) return;
        if (s_bh_day_idx < s_bh_n_dates - 1) s_bh_day_idx++;
        else                                 s_bh_day_idx = -1;
    }
    s_bh_win_a = 0.0f; s_bh_win_b = 1.0f;
    /* Volver a HOY relee: su mitad viva (el anillo) y su CSV han seguido
     * creciendo mientras se miraban otros dias. Los dias pasados no cambian,
     * asi que esos si valen tal cual desde los buffers. */
    if (s_bh_day_idx < 0) s_bh_loaded_idx = -2;
    bh_update_zoom_label();
    bh_chart_load_day();
}

static void bh_arrow_cb(lv_event_t *e)
{
    bh_step_day(lv_event_get_user_data(e) != NULL);
}

static void bh_chart_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    /* Zoomeado: el pan se hace arrastrando (bh_chart_touch_cb); ignoramos el
     * swipe para no cambiar de dia sin querer. */
    bool zoomed = (s_bh_win_a > 0.0001f) || (s_bh_win_b < 0.9999f);
    if (zoomed) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT)     bh_step_day(true);
    else if (dir == LV_DIR_LEFT) bh_step_day(false);
}

static void bh_update_zoom_label(void)
{
    if (!s_bh_lbl_zoom) return;
    float w = s_bh_win_b - s_bh_win_a;
    if (w <= 0.0f) w = 1.0f;
    float z = 1.0f / w;
    if (z < 1.05f) lv_label_set_text(s_bh_lbl_zoom, "1x");
    else if (z < 9.5f) lv_label_set_text_fmt(s_bh_lbl_zoom, "%.1fx", z);
    else               lv_label_set_text_fmt(s_bh_lbl_zoom, "%dx", (int)(z + 0.5f));
}

static void bh_apply_window(void)
{
    /* Clamp + sanidad */
    if (s_bh_win_a < 0.0f) s_bh_win_a = 0.0f;
    if (s_bh_win_b > 1.0f) s_bh_win_b = 1.0f;
    if (s_bh_win_b - s_bh_win_a < 0.005f) {  /* zoom max ~200x */
        float c = (s_bh_win_a + s_bh_win_b) * 0.5f;
        s_bh_win_a = c - 0.0025f;
        s_bh_win_b = c + 0.0025f;
        if (s_bh_win_a < 0) { s_bh_win_b -= s_bh_win_a; s_bh_win_a = 0; }
        if (s_bh_win_b > 1) { s_bh_win_a -= (s_bh_win_b - 1); s_bh_win_b = 1; }
    }
    bh_update_zoom_label();
    bh_chart_load_day();
}

/* Gesto tactil sobre el chart: arrastrar = desplazar (pan), doble-toque =
 * zoom in centrado en el punto, mantener pulsado = volver a 1x. El cambio de
 * dia se hace con swipe (gesture_cb) solo cuando estamos en 1x. */
static void bh_chart_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_bh_drag_last_x = p.x;
        s_bh_drag_moved  = 0;
        s_bh_dragging    = false;
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dx = p.x - s_bh_drag_last_x;
        s_bh_drag_last_x = p.x;
        s_bh_drag_moved += dx < 0 ? -dx : dx;
        if (s_bh_drag_moved < 6) return;      /* umbral para no confundir tap */
        s_bh_dragging = true;
        float win_w = s_bh_win_b - s_bh_win_a;
        lv_area_t ca; lv_obj_get_content_coords(s_bh_chart, &ca);
        int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
        float shift = -(float)dx / (float)cw * win_w;
        s_bh_win_a += shift; s_bh_win_b += shift;
        if (s_bh_win_a < 0.0f) { s_bh_win_b -= s_bh_win_a; s_bh_win_a = 0.0f; }
        if (s_bh_win_b > 1.0f) { s_bh_win_a -= (s_bh_win_b - 1.0f); s_bh_win_b = 1.0f; }
        int64_t now = esp_timer_get_time();
        if (now - s_bh_last_apply_us > 120000) {  /* throttle recarga ~8/s */
            s_bh_last_apply_us = now;
            bh_update_zoom_label();
            bh_chart_load_day();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_bh_dragging) {
            bh_update_zoom_label();
            bh_chart_load_day();              /* recarga final tras soltar */
            s_bh_dragging = false;
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (!s_bh_dragging) {                 /* mantener pulsado = reset a 1x */
            s_bh_win_a = 0.0f; s_bh_win_b = 1.0f;
            bh_apply_window();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_bh_dragging) return;
        int64_t now = esp_timer_get_time();
        if (now - s_bh_last_click_us < 350000) {   /* doble-toque = zoom in */
            s_bh_last_click_us = 0;
            float win_w = s_bh_win_b - s_bh_win_a;
            lv_area_t ca; lv_obj_get_content_coords(s_bh_chart, &ca);
            int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
            float rel = (float)(p.x - ca.x1) / (float)cw;
            if (rel < 0) rel = 0;
            if (rel > 1) rel = 1;
            float center = s_bh_win_a + rel * win_w;
            float nw = win_w * 0.5f;
            s_bh_win_a = center - rel * nw;
            s_bh_win_b = s_bh_win_a + nw;
            bh_apply_window();
        } else {
            s_bh_last_click_us = now;
        }
    }
}
