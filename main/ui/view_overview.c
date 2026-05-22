#include "view_overview.h"
#include "ui_card.h"
#include "energy_today.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "ui.h"
#include "ne185/ne185.h"
#include "frigo.h"
#include "audio_es8311.h"
#include "alerts.h"
#include "lv_font_thermometer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ui_device_view_t base;
    /* Cards */
    lv_obj_t *card_solar;
    lv_obj_t *m_solar_w;
    /* Card central Bateria */
    lv_obj_t *card_bat;
    lv_obj_t *arc_soc;
    lv_obj_t *m_bat_current;
    /* Card DC/DC (Orion-Tr Smart, carga desde batería motor) */
    lv_obj_t *card_loads;      /* nombre mantenido por compatibilidad */
    lv_obj_t *m_loads_w;       /* V_in: tensión batería motor */
    /* Métricas extra dentro de cada card */
    lv_obj_t *m_ttg;          /* dentro de card_bat: Autonomía */
    lv_obj_t *m_solar_kwh;    /* dentro de card_solar: kWh hoy */
    lv_obj_t *m_loads_kwh;    /* dentro de card_dcdc: V_out (servicio) */

    /* State-store DC/DC */
    struct {
        bool     has_data;
        uint8_t  state;       /* device_state: 0=off, otros valores=activos */
        int16_t  vin_centi;   /* tension entrada (bateria motor) */
        int16_t  vout_centi;  /* tension salida (bateria servicio) */
        uint32_t last_update_ms;
    } dcdc;

    /* State-store consolidado por tipo */
    struct {
        bool has_data;
        uint16_t soc_deci;
        uint16_t voltage_centi;
        int32_t current_milli;
        uint32_t ttg_min;
        uint32_t last_update_ms;
    } bat;
    struct {
        bool has_data;
        uint16_t pv_w;
        int16_t  load_current_deci;
        uint16_t voltage_centi;
        uint32_t last_update_ms;
    } solar;

    /* ── Widgets camper (NE185 via UART) ─────────────────────── */
    lv_obj_t *tank_s1;           /* tanque agua limpia (debajo de Solar) */
    lv_obj_t *tank_r1;           /* tanque aguas grises (debajo de DC/DC) */
    lv_obj_t *pill_shore;        /* indicador 230 V (debajo de Bateria) */
    lv_obj_t *pill_shore_lbl;    /* texto interno del pill (ON/OFF) */
    /* ── Widgets frigo (DS18B20 + ventilador PWM) ─────────────── */
    lv_obj_t *lbl_freezer_temp;  /* T_Congelador (junto a tank limpia) */
    lv_obj_t *img_fan;           /* icono ventilador (animado, debajo 230V) */
    int       fan_angle_deci;    /* angulo actual rotacion (0..3599) */
    /* ── Alarmas (S1 vacio / R1 lleno / SOC < 30 % / Frigo > umbral) ── */
    bool      alarm_s1_muted;
    bool      alarm_r1_muted;
    bool      alarm_soc_muted;       /* mute al pulsar la card de bateria */
    bool      alarm_freezer_muted;   /* mute al pulsar la temp congelador */
    bool      prev_alarm_s1;
    bool      prev_alarm_r1;
    bool      prev_alarm_soc;
    bool      prev_alarm_freezer;
    uint32_t  alarm_s1_last_sound_ms;
    uint32_t  alarm_r1_last_sound_ms;
    uint32_t  alarm_soc_last_sound_ms;
    uint32_t  alarm_freezer_last_sound_ms;
    uint8_t   blink_phase;           /* alterna 0/1 cada tick para parpadeo */
    lv_obj_t *camper_bottom;     /* contenedor de los 3 botones */
    lv_obj_t *btn_lin;
    lv_obj_t *btn_lout;
    lv_obj_t *btn_pump;
} ui_overview_view_t;

static void overview_update(ui_device_view_t *view, const victron_data_t *data);
static void overview_show(ui_device_view_t *view);
static void overview_hide(ui_device_view_t *view);
static void overview_destroy(ui_device_view_t *view);
static void overview_render(ui_overview_view_t *ov);
static uint32_t now_ms(void) { return lv_tick_get(); }

static lv_obj_t *create_node_card(lv_obj_t *parent, const lv_img_dsc_t *img,
                                  const char *title, lv_color_t accent,
                                  lv_obj_t **out_metric)
{
    lv_obj_t *card = ui_card_create(parent, accent);
    /* Header con icono raster + metric debajo. El caller fija altura/grow */
    ui_card_set_title_img(card, img, title, accent);
    if (out_metric) {
        *out_metric = ui_metric_create_compact(card, "");
    }
    return card;
}

/* ───────────────────────────────────────────────────────────────
 * Widgets camper (NE185): barra superior con tanques + 230V,
 * fila inferior con 3 botones (luz int, luz ext, bomba).
 * ─────────────────────────────────────────────────────────────── */
/* ── Sistema de alarmas tanques: cola + tarea para el pitido de 5 s ── */
static QueueHandle_t s_alarm_queue = NULL;

/* Patron de alarma estilo "detector de humos" (~5 s).
 * Es el sonido universalmente reconocido como ALARMA: tres pitidos
 * agudos cortos, silencio, repetido. Sin musicalidad ni adornos —
 * directamente identificable como aviso de emergencia. */
static const audio_note_t s_alarm_pattern[] = {
    /* Triple beep #1 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple beep #2 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple beep #3 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple beep final */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120},
};

static void overview_alarm_task(void *arg)
{
    (void)arg;
    uint8_t v;
    while (1) {
        if (xQueueReceive(s_alarm_queue, &v, portMAX_DELAY) == pdTRUE) {
            /* Subir al maximo solo durante el pitido y restaurar despues
             * para no afectar el volumen normal del usuario. */
            int prev_vol = audio_get_volume();
            audio_set_volume(100);
            audio_play_tones(s_alarm_pattern,
                             sizeof(s_alarm_pattern) / sizeof(s_alarm_pattern[0]));
            audio_set_volume(prev_vol);
        }
    }
}

/* Callbacks de click en tanques para silenciar la alarma correspondiente.
 * audio_cancel_playback corta la reproduccion en curso en < 50 ms. */
static void alarm_mute_s1_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    if (ov) ov->alarm_s1_muted = true;
    audio_cancel_playback();
}
static void alarm_mute_r1_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    if (ov) ov->alarm_r1_muted = true;
    audio_cancel_playback();
}
static void alarm_mute_soc_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    if (ov) ov->alarm_soc_muted = true;
    audio_cancel_playback();
}
static void alarm_mute_freezer_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    if (ov) ov->alarm_freezer_muted = true;
    audio_cancel_playback();
}

static void overview_camper_tick_cb(lv_timer_t *t)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov || !ov->base.root) return;
    if (lv_obj_has_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN)) return;
    /* No renderizar mientras el screensaver esta activo (overlays encima
     * o brillo bajo); evita consumir heap LVGL haciendo redraws invisibles. */
    if (ov->base.ui && ov->base.ui->screensaver.active) return;
    overview_render(ov);
}

/* Timer aparte a 50 ms solo para la animacion de rotacion del icono
 * del ventilador. La velocidad de giro se escala con fan_percent:
 *   100 % → ~36 deg/s = una vuelta cada 10 segundos.
 *     0 % → no se mueve. */
static void overview_fan_rotate_cb(lv_timer_t *t)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov || !ov->img_fan) return;
    if (lv_obj_has_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN)) return;
    /* Igual que en camper_tick_cb: si el screensaver esta activo no rotamos
     * la imagen para no agotar el heap LVGL con buffers de rotacion. */
    if (ov->base.ui && ov->base.ui->screensaver.active) return;
    const frigo_state_t *fs = frigo_get_state();
    if (!fs) return;
    uint8_t p = fs->fan_percent;
    if (p == 0) return;
    /* delta_deci en decimas de grado por cada 50 ms. A 100 % giramos
     * 90 deci/tick = 9 deg/tick = 180 deg/s = una vuelta cada 2 segundos.
     * A 50 % seria 4.5 deg/tick = 90 deg/s = 4 segundos por vuelta. */
    /* delta_deci en decimas de grado por tick (150 ms).
     * A 100 % -> 270 deci/tick = 27 deg/tick = 180 deg/s (mismo que antes). */
    int delta = (p * 270) / 100;
    ov->fan_angle_deci += delta;
    if (ov->fan_angle_deci >= 3600) ov->fan_angle_deci -= 3600;
    lv_img_set_angle(ov->img_fan, ov->fan_angle_deci);
}

static void camper_btn_event_cb(lv_event_t *e)
{
    char cmd = (char)(intptr_t)lv_event_get_user_data(e);
    ne185_send_cmd(cmd);
}

/* (helper camper_make_tank antiguo eliminado: ahora se usa
 *  ui_tank_create de ui_card.c, que es el widget visual grande) */

/* DEMO temporal de tanques: cicla niveles 0..4 cada tick (1.5 s).
 * tank_s1 (CLEAN_H) muestra cada nivel del NE187; tank_r1 (GREY_H)
 * alterna OK/LLENO según paridad. user_data = ui_overview_view_t*. */
static void tank_demo_cb(lv_timer_t *t)
{
    static uint8_t step;
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov) return;
    /* Demo solo para grises (limpia espera datos reales del NE185). */
    if (ov->tank_r1) ui_tank_set(ov->tank_r1, step % 2);
    step = (step + 1) % 5;
}

/* Crea un botón "píldora" con icono + texto y un LED indicador en la esquina
 * superior derecha. El LED queda accesible via lv_obj_get_user_data(btn).
 * El `accent` se usa como color del borde (2 px) para distinguir la funcion. */
static lv_obj_t *camper_make_button(lv_obj_t *parent,
                                    const char *icon, const char *text,
                                    char cmd_char, lv_color_t accent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 200, 85);
    lv_obj_set_style_radius(btn, 42, 0);                 /* píldora: radius = h/2 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, 0);
    /* Borde de color de la funcion (2 px) — ahora SE VE como boton */
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    /* Sombra suave para sensación de elevación */
    lv_obj_set_style_shadow_width(btn, 14, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 4, 0);

    /* Contenedor horizontal con icono (fuente regular para los glyphs FA)
     * + texto (SemiBold para mejor contraste sin oscurecer el fondo). */
    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_center(row);

    if (icon && icon[0]) {
        lv_obj_t *l_icon = lv_label_create(row);
        lv_obj_set_style_text_font(l_icon, &lv_font_montserrat_28_es, 0);
        lv_obj_set_style_text_color(l_icon, accent, 0);
        lv_label_set_text(l_icon, icon);
    }
    lv_obj_t *l_text = lv_label_create(row);
    lv_obj_set_style_text_font(l_text, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(l_text, accent, 0);
    lv_label_set_text(l_text, text ? text : "");

    /* LED indicador 10x10 esquina sup. dcha. Off gris oscuro; on cambia
     * color + halo en el refresh general. */
    lv_obj_t *led = lv_obj_create(btn);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, 14, 14);
    lv_obj_set_style_radius(led, 7, 0);
    lv_obj_set_style_bg_color(led, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    /* LED centrado horizontalmente, asomando un poco por encima del borde */
    lv_obj_align(led, LV_ALIGN_TOP_MID, 0, -5);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(btn, led);

    lv_obj_add_event_cb(btn, camper_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)cmd_char);
    return btn;
}

ui_device_view_t *ui_overview_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (!ui || !parent) return NULL;
    ui_overview_view_t *ov = calloc(1, sizeof(*ov));
    if (!ov) return NULL;

    ov->base.ui = ui;
    ov->base.root = lv_obj_create(parent);
    lv_obj_set_size(ov->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(ov->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov->base.root, 0, 0);
    lv_obj_set_style_pad_all(ov->base.root, 8, 0);
    lv_obj_set_style_pad_gap(ov->base.root, 8, 0);
    lv_obj_set_layout(ov->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ov->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov->base.root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ov->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN);

    /* ── Grid principal: 3 columnas verticales ──────────────── */
    lv_obj_t *grid = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Columna izquierda: card Solar arriba + tanque limpia abajo ── */
    lv_obj_t *col_left = lv_obj_create(grid);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_height(col_left, lv_pct(100));
    lv_obj_set_flex_grow(col_left, 3);
    lv_obj_set_layout(col_left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_left, 8, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_solar = create_node_card(col_left, &icon_solar,
                                      "Solar", UI_COLOR_GREEN, &ov->m_solar_w);
    lv_obj_set_width(ov->card_solar, lv_pct(100));
    /* Altura fija para garantizar tamaño igual al de bat / dcdc.
     * flex_grow=0 + height pct(80) → cada card 80% del alto de la col. */
    lv_obj_set_flex_grow(ov->card_solar, 0);
    lv_obj_set_height(ov->card_solar, lv_pct(80));
    /* Subir el contenido 5 px sin alterar el tamano externo de la card:
     * reducimos el pad superior y aumentamos el inferior en la misma
     * cantidad. La altura es la misma porque la fija el flex_grow. */
    lv_obj_set_style_pad_top(ov->card_solar, UI_PAD_CARD - 5, 0);
    lv_obj_set_style_pad_bottom(ov->card_solar, UI_PAD_CARD + 5, 0);
    /* Reducir el gap entre header/Actual/Hoy de 16 a 8 px: la 2ª métrica
     * se salía por abajo en columnas estrechas. */
    lv_obj_set_style_pad_gap(ov->card_solar, 8, 0);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_solar_w, "Actual", UI_COLOR_TEXT_DIM);
    ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT);
    ov->m_solar_kwh = ui_metric_create_compact(ov->card_solar, "Hoy");
    ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);

    /* Barra horizontal estilo LED bargraph (como NE187). Ocupa todo el
     * ancho de la columna y el alto restante (col_left = 8+2 = 10). */
    ov->tank_s1 = ui_tank_create(col_left, lv_pct(95), 1,
                                 "Agua limpia", UI_COLOR_CYAN, UI_TANK_CLEAN_H);
    lv_obj_set_flex_grow(ov->tank_s1, 2);
    lv_obj_add_flag(ov->tank_s1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->tank_s1, alarm_mute_s1_cb, LV_EVENT_CLICKED, ov);

    /* ── Columna central: card Bateria arriba + indicador 230V abajo ── */
    lv_obj_t *col_center = lv_obj_create(grid);
    lv_obj_remove_style_all(col_center);
    lv_obj_set_height(col_center, lv_pct(100));
    lv_obj_set_flex_grow(col_center, 5);
    lv_obj_set_layout(col_center, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_center, 8, 0);
    lv_obj_clear_flag(col_center, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_bat = ui_card_create(col_center, UI_COLOR_ORANGE);
    lv_obj_set_width(ov->card_bat, lv_pct(100));
    /* 8:3 igual que los laterales (col_left card_solar=8, tank_s1=3 y
     * col_right card_loads=8, tank_r1=3) para que card_bat tenga la misma
     * altura que Solar/DC/DC y el bottom_row gane espacio suficiente para
     * que la card frigo se vea entera. */
    lv_obj_set_flex_grow(ov->card_bat, 0);
    lv_obj_set_height(ov->card_bat, lv_pct(80));
    /* Subir el contenido 10 px en total (los 5 px del cambio anterior +
     * 5 px adicionales pedidos al "subir la card 5"). Reducimos pad_top
     * a UI_PAD_CARD-10 y aumentamos pad_bottom a UI_PAD_CARD+10 para no
     * tocar el tamano externo de la card. */
    lv_obj_set_style_pad_top(ov->card_bat, UI_PAD_CARD - 10, 0);
    lv_obj_set_style_pad_bottom(ov->card_bat, UI_PAD_CARD + 10, 0);
    {
        /* Captamos el header para acceder al icono y subirlo un poco
         * verticalmente (estructura: header -> left(flex row) -> img + lbl). */
        lv_obj_t *bat_header = ui_card_set_title_img(ov->card_bat, &icon_battery,
                                                     "Batería", UI_COLOR_ORANGE);
        lv_obj_t *bat_left = bat_header ? lv_obj_get_child(bat_header, 0) : NULL;
        lv_obj_t *bat_img  = bat_left   ? lv_obj_get_child(bat_left, 0) : NULL;
        if (bat_img) {
            lv_obj_set_style_translate_y(bat_img, -6, 0);
        }
    }
    /* Click en la card de bateria silencia la alarma de SOC */
    lv_obj_add_flag(ov->card_bat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->card_bat, alarm_mute_soc_cb, LV_EVENT_CLICKED, ov);
    /* Cuerpo de card_bat: fila horizontal con [Corriente · Arc SoC · Autonomía].
     * Aprovecha el ancho disponible en lugar del alto para que con la card
     * a 8:3 (igual altura que Solar/DC/DC) las métricas no se recorten. */
    lv_obj_t *bat_row = lv_obj_create(ov->card_bat);
    lv_obj_remove_style_all(bat_row);
    lv_obj_set_width(bat_row, lv_pct(100));
    lv_obj_set_flex_grow(bat_row, 1);
    lv_obj_set_layout(bat_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bat_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_row, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bat_row, LV_OBJ_FLAG_SCROLLABLE);
    ov->m_bat_current = ui_metric_create_compact(bat_row, "Corriente");
    ov->arc_soc       = ui_battery_soc_create(bat_row, 120, 112);
    ov->m_ttg         = ui_metric_create_compact(bat_row, "Autonomía");
    /* Bajar fuentes para que las metricas no invadan el arc SoC en el ancho
     * disponible (col_center ~320 px, arc 120 px ⇒ ~95 px por metrica).
     * title 24→20, value 46→32, unit 24→20. Desplazar 10 px hacia abajo. */
    {
        lv_obj_t *metrics[] = { ov->m_bat_current, ov->m_ttg };
        for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
            lv_obj_t *title = lv_obj_get_child(metrics[i], 0);
            lv_obj_t *row   = lv_obj_get_child(metrics[i], 1);
            if (title) lv_obj_set_style_text_font(title, &lv_font_montserrat_20_es, 0);
            if (row) {
                lv_obj_t *value = lv_obj_get_child(row, 0);
                lv_obj_t *unit  = lv_obj_get_child(row, 1);
                if (value) lv_obj_set_style_text_font(value, &lv_font_montserrat_32, 0);
                if (unit)  lv_obj_set_style_text_font(unit,  &lv_font_montserrat_20_es, 0);
            }
            lv_obj_set_style_translate_y(metrics[i], 48, 0);
        }
    }

    /* Tank aguas grises directamente en col_center (sin wrapper), con
     * mismo flex_grow=2 que tank_s1 en col_left → mismo alto exacto. */
    ov->tank_r1 = ui_tank_create(col_center, lv_pct(40), 1,
                                 "Aguas grises", UI_COLOR_CYAN, UI_TANK_GREY_H);
    lv_obj_set_flex_grow(ov->tank_r1, 2);
    lv_obj_add_flag(ov->tank_r1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->tank_r1, alarm_mute_r1_cb, LV_EVENT_CLICKED, ov);

    /* ── Columna derecha: card DC/DC arriba + tanque grises abajo ── */
    lv_obj_t *col_right = lv_obj_create(grid);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_height(col_right, lv_pct(100));
    lv_obj_set_flex_grow(col_right, 3);
    lv_obj_set_layout(col_right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_right, 8, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

    ov->card_loads = create_node_card(col_right, &icon_dcdc,
                                      "DC/DC", UI_COLOR_CYAN, &ov->m_loads_w);
    lv_obj_set_width(ov->card_loads, lv_pct(100));
    lv_obj_set_flex_grow(ov->card_loads, 0);
    lv_obj_set_height(ov->card_loads, lv_pct(80));
    lv_obj_set_style_pad_top(ov->card_loads, UI_PAD_CARD - 5, 0);
    lv_obj_set_style_pad_bottom(ov->card_loads, UI_PAD_CARD + 5, 0);
    /* Reducir el gap entre header/Motor/Servicio de 16 a 8 px: la 2ª
     * métrica se salía por abajo en columnas estrechas. */
    lv_obj_set_style_pad_gap(ov->card_loads, 8, 0);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_loads_w, "Motor", UI_COLOR_TEXT_DIM);
    ui_metric_set(ov->m_loads_w, "--", "V", UI_COLOR_TEXT);
    ov->m_loads_kwh = ui_metric_create_compact(ov->card_loads, "Servicio");
    ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);

    /* Pill 230V (intercambiado con tank_r1 que ahora va bajo card_bat). */
    {
        lv_obj_t *wrap = lv_obj_create(col_right);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_width(wrap, lv_pct(100));
        lv_obj_set_flex_grow(wrap, 2);
        lv_obj_set_layout(wrap, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *pill = lv_obj_create(wrap);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(pill, 14, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_border_color(pill, UI_COLOR_CARD_BORDER, 0);
        lv_obj_set_style_bg_color(pill, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 16, 0);
        lv_obj_set_style_pad_ver(pill, 8, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        ov->pill_shore_lbl = lv_label_create(pill);
        lv_obj_set_style_text_font(ov->pill_shore_lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(ov->pill_shore_lbl, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->pill_shore_lbl, "230 V");
        lv_obj_center(ov->pill_shore_lbl);
        ov->pill_shore = pill;
    }

    /* col_right = 8+2 = 10 igual que col_left y col_center → las 3 cards
     * (Solar, Bateria, DC/DC) ocupan exactamente 8/10 = 80% del alto */

    /* ── Camper bottom: [ [Luz INT][Bomba]  ........  [Luz EXT] ] ──
     * Agrupamos Luz INT + Bomba en un sub-grupo a la izquierda y dejamos
     * Luz EXT solo a la derecha (SPACE_BETWEEN en el contenedor padre). */
    ov->camper_bottom = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(ov->camper_bottom);
    lv_obj_set_width(ov->camper_bottom, lv_pct(100));
    lv_obj_set_height(ov->camper_bottom, LV_SIZE_CONTENT);
    lv_obj_set_layout(ov->camper_bottom, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ov->camper_bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ov->camper_bottom, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ov->camper_bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(ov->camper_bottom, 6, 0);
    lv_obj_set_style_pad_hor(ov->camper_bottom, 8, 0);

    /* Grupo izquierdo: Luz INT + Bomba pegados entre sí (gap pequeño). */
    lv_obj_t *left_group = lv_obj_create(ov->camper_bottom);
    lv_obj_remove_style_all(left_group);
    lv_obj_set_size(left_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(left_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left_group, 20, 0);
    lv_obj_clear_flag(left_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Icono bombilla FA5 (0xF0EB) en UTF-8 = "\xEF\x83\xAB" */
    ov->btn_lin  = camper_make_button(left_group, "\xEF\x83\xAB", "Luz INT", 'i',
                                       UI_COLOR_YELLOW);
    ov->btn_pump = camper_make_button(left_group, LV_SYMBOL_TINT, "Bomba",   'p',
                                       UI_COLOR_CYAN);

    /* Luz EXT en posicion central (swap con la card del congelador
     * a peticion del usuario 2026-05-22). Creando btn_lout ANTES que
     * card_fridge, con SPACE_BETWEEN del camper_bottom queda:
     *   [LIN+Bomba] [Luz EXT] [card_fridge]
     */
    ov->btn_lout = camper_make_button(ov->camper_bottom, "\xEF\x83\xAB", "Luz EXT", 'o',
                                       UI_COLOR_YELLOW);

    /* Card frigorífico (marco cian) a la derecha de camper_bottom. */
    {
        lv_obj_t *card_fridge = lv_obj_create(ov->camper_bottom);
        lv_obj_remove_style_all(card_fridge);
        lv_obj_set_size(card_fridge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card_fridge, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(card_fridge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card_fridge, UI_COLOR_CYAN, 0);
        lv_obj_set_style_border_width(card_fridge, 2, 0);
        lv_obj_set_style_radius(card_fridge, UI_RADIUS_CARD, 0);
        lv_obj_set_style_pad_hor(card_fridge, 14, 0);
        lv_obj_set_style_pad_ver(card_fridge, 6, 0);
        lv_obj_set_style_pad_gap(card_fridge, 14, 0);
        lv_obj_set_layout(card_fridge, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card_fridge, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card_fridge, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card_fridge, LV_OBJ_FLAG_SCROLLABLE);

        /* Congelador (izquierda dentro de la card) — clickable para
         * silenciar la alarma. */
        lv_obj_t *col_freezer = lv_obj_create(card_fridge);
        lv_obj_remove_style_all(col_freezer);
        lv_obj_set_size(col_freezer, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(col_freezer, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(col_freezer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col_freezer, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(col_freezer, 2, 0);
        lv_obj_add_flag(col_freezer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(col_freezer, alarm_mute_freezer_cb,
                            LV_EVENT_CLICKED, ov);

        lv_obj_t *t_row = lv_obj_create(col_freezer);
        lv_obj_remove_style_all(t_row);
        lv_obj_set_size(t_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(t_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(t_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(t_row, 4, 0);

        static lv_font_t s_font_thermo_no_fb;
        s_font_thermo_no_fb = lv_font_thermometer;
        s_font_thermo_no_fb.fallback = NULL;
        lv_obj_t *t_icon = lv_label_create(t_row);
        lv_obj_set_style_text_font(t_icon, &s_font_thermo_no_fb, 0);
        lv_obj_set_style_text_color(t_icon, UI_COLOR_CYAN, 0);
        lv_label_set_text(t_icon, "\xef\x8b\x89");

        lv_obj_t *t_lbl = lv_label_create(t_row);
        lv_obj_set_style_text_font(t_lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(t_lbl, UI_COLOR_CYAN, 0);
        lv_label_set_text(t_lbl, "Congelador");
        ov->lbl_freezer_temp = lv_label_create(col_freezer);
        lv_obj_set_style_text_font(ov->lbl_freezer_temp, &lv_font_montserrat_28_es, 0);
        lv_obj_set_style_text_color(ov->lbl_freezer_temp, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->lbl_freezer_temp, "-- \xc2\xb0""C");

        /* Ventilador (derecha dentro de la card) */
        ov->img_fan = lv_img_create(card_fridge);
        lv_img_set_src(ov->img_fan, &icon_fan);
        lv_img_set_pivot(ov->img_fan, 40, 40);
        lv_obj_set_style_img_recolor(ov->img_fan, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_img_recolor_opa(ov->img_fan, LV_OPA_COVER, 0);
        ov->fan_angle_deci = 0;
    }

    /* btn_lout creado mas arriba (antes del card_fridge tras swap). */

    /* Timer LVGL para refrescar los widgets camper aunque no llegue
     * dato Victron. Cada 500 ms re-renderiza la vista. */
    lv_timer_create(overview_camper_tick_cb, 500, ov);
    lv_timer_create(overview_fan_rotate_cb,  150, ov);

    /* Cola y tarea para la alarma sonora de 5 s (no bloquea LVGL). */
    if (!s_alarm_queue) {
        s_alarm_queue = xQueueCreate(1, sizeof(uint8_t));
        if (s_alarm_queue) {
            xTaskCreate(overview_alarm_task, "ov_alarm", 3072, NULL, 4, NULL);
        }
    }

    /* Defaults */
    ov->bat.ttg_min = 0xFFFFFFFF;

    ov->base.update  = overview_update;
    ov->base.show    = overview_show;
    ov->base.hide    = overview_hide;
    ov->base.destroy = overview_destroy;

    overview_render(ov);

    /* Tanques: ahora decodificados desde NE185 (clean = b[5] nibble bajo,
     * grey = b[6] nibble bajo). El refresco de los niveles llega via
     * overview_update -> ne185_data_t.s1/r1, sin necesidad de timer demo. */

    return &ov->base;
}

static void overview_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)view;
    if (!ov) return;
    uint32_t now = now_ms();

    if (data) {
        switch (data->type) {
            case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
                const victron_record_battery_monitor_t *b = &data->record.battery;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = b->battery_current_milli;
                ov->bat.ttg_min = b->time_to_go_minutes;
                ov->bat.last_update_ms = now;
                energy_today_on_battery(b->battery_current_milli,
                                        b->battery_voltage_centi);
                ui_card_pulse(ov->card_bat);
                break;
            }
            case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
                const victron_record_dcdc_converter_t *c = &data->record.dcdc;
                ov->dcdc.has_data = true;
                ov->dcdc.state      = c->device_state;
                ov->dcdc.vin_centi  = (int16_t)c->input_voltage_centi;
                ov->dcdc.vout_centi = (int16_t)c->output_voltage_centi;
                ov->dcdc.last_update_ms = now;
                ui_card_pulse(ov->card_loads);
                break;
            }
            case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
                const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = (int32_t)b->battery_current_deci * 100;
                ov->bat.ttg_min = b->time_to_go_min;
                ov->bat.last_update_ms = now;
                ui_card_pulse(ov->card_bat);
                break;
            }
            case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
                const victron_record_solar_charger_t *s = &data->record.solar;
                ov->solar.has_data = true;
                ov->solar.pv_w = s->pv_power_w;
                ov->solar.load_current_deci = s->load_current_deci;
                ov->solar.voltage_centi = s->battery_voltage_centi;
                ov->solar.last_update_ms = now;
                energy_today_on_solar_yield(s->yield_today_centikwh);
                ui_card_pulse(ov->card_solar);
                /* Si no hay BMV, también usamos los datos de batería del Solar */
                if (!ov->bat.has_data) {
                    ov->bat.voltage_centi = s->battery_voltage_centi;
                    ov->bat.current_milli = (int32_t)s->battery_current_deci * 100;
                    ov->bat.last_update_ms = now;
                }
                break;
            }
            default: break;
        }
    }

    overview_render(ov);
}

static void overview_render(ui_overview_view_t *ov)
{
    if (!ov) return;
    uint32_t now = now_ms();
    const uint32_t TIMEOUT_MS = 30000;
    char buf[24];

    bool bat_fresh   = ov->bat.has_data &&
                       (now - ov->bat.last_update_ms) < TIMEOUT_MS;
    bool solar_fresh = ov->solar.has_data &&
                       (now - ov->solar.last_update_ms) < TIMEOUT_MS;

    /* ── Solar A (corriente de carga PV→batería) ──────────────── */
    /* En SmartSolar BLE, battery_current_deci está en décimas de A
     * sobre el solar.pv_w (potencia). Si no tenemos battery_current_deci
     * publicado en el state-store, derivamos A = W/V. */
    long solar_centi_a = -1;  /* en cA (centi-amperios) */
    if (solar_fresh && ov->solar.voltage_centi > 0) {
        /* I = P/V → centi-A = W * 10000 / voltage_centi */
        solar_centi_a = ((long)ov->solar.pv_w * 10000L) /
                        (long)ov->solar.voltage_centi;
    }
    if (solar_centi_a >= 0) {
        snprintf(buf, sizeof(buf), "%ld.%02ld",
                 solar_centi_a / 100, solar_centi_a % 100);
        ui_metric_set(ov->m_solar_w, buf, "A",
                      solar_centi_a > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT_DIM);
    }

    /* (Las flechas Solar→Bat / Bat→DC se eliminaron al pasar a layout
     * de 3 columnas verticales. El flujo se infiere de los colores.) */

    /* ── Batería: SOC + corriente ──────────────────────────────── */
    if (bat_fresh) {
        ui_battery_soc_set(ov->arc_soc, ov->bat.soc_deci, ov->bat.voltage_centi);
        int32_t mi = ov->bat.current_milli;
        int abs_a_centi = mi < 0 ? -mi/10 : mi/10;
        snprintf(buf, sizeof(buf), "%c%d.%02d",
                 mi >= 0 ? '+' : '-', abs_a_centi/100, abs_a_centi%100);
        ui_metric_set(ov->m_bat_current, buf, "A", ui_color_for_current(mi));
    } else {
        ui_battery_soc_set(ov->arc_soc, 0xFFFF, 0);
        ui_metric_set(ov->m_bat_current, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── DC/DC (Orion-Tr Smart): muestra V_in (motor) y V_out (servicio) ── */
    bool dcdc_fresh = ov->dcdc.has_data &&
                      (now - ov->dcdc.last_update_ms) < TIMEOUT_MS;
    if (dcdc_fresh) {
        /* V_in (batería motor) — métrica principal grande */
        snprintf(buf, sizeof(buf), "%u.%02u",
                 ov->dcdc.vin_centi / 100, ov->dcdc.vin_centi % 100);
        bool active = ov->dcdc.state != 0;
        ui_metric_set(ov->m_loads_w, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
        /* V_out (batería servicio) — métrica pequeña */
        snprintf(buf, sizeof(buf), "%u.%02u",
                 ov->dcdc.vout_centi / 100, ov->dcdc.vout_centi % 100);
        ui_metric_set(ov->m_loads_kwh, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_loads_w,   "--", "V", UI_COLOR_TEXT_DIM);
        ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);
    }

    /* ── TTG ──────────────────────────────────────────────────── */
    if (bat_fresh && ov->bat.ttg_min != 0xFFFFFFFF && ov->bat.ttg_min > 0) {
        if (ov->bat.ttg_min >= 1440) {
            /* ≥ 24h → días y horas */
            snprintf(buf, sizeof(buf), "%ud %uh",
                     (unsigned)(ov->bat.ttg_min / 1440),
                     (unsigned)((ov->bat.ttg_min % 1440) / 60));
        } else if (ov->bat.ttg_min >= 60) {
            snprintf(buf, sizeof(buf), "%uh %02um",
                     (unsigned)(ov->bat.ttg_min / 60),
                     (unsigned)(ov->bat.ttg_min % 60));
        } else {
            snprintf(buf, sizeof(buf), "%um", (unsigned)ov->bat.ttg_min);
        }
        ui_metric_set(ov->m_ttg, buf, "", UI_COLOR_TEXT);
    } else {
        ui_metric_set(ov->m_ttg, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── Camper (NE185 vía UART) ─────────────────────────────── */
    {
        ne185_data_t cd;
        ne185_get(&cd);

        /* Tanques visuales: nivel 0..3 (ui_tank_set ignora valores > 3) */
        if (ov->tank_s1) ui_tank_set(ov->tank_s1, cd.fresh ? cd.s1 : 0xFF);
        if (ov->tank_r1) ui_tank_set(ov->tank_r1, cd.fresh ? cd.r1 : 0xFF);

        /* ── Alarmas de tanque ─────────────────────────────── */
        bool alarm_s1 = cd.fresh && cd.s1 == 0;
        bool alarm_r1 = cd.fresh && cd.r1 == 3;
        uint32_t now_ms_val = (uint32_t)(lv_tick_get());

        /* Auto-reset del mute al volver a estado normal */
        if (!alarm_s1 && ov->prev_alarm_s1) ov->alarm_s1_muted = false;
        if (!alarm_r1 && ov->prev_alarm_r1) ov->alarm_r1_muted = false;
        ov->prev_alarm_s1 = alarm_s1;
        ov->prev_alarm_r1 = alarm_r1;
        ov->blink_phase ^= 1;

        /* Parpadeo visual: alternar opacidad cada 500 ms si alarma activa
         * y no silenciada. */
        if (ov->tank_s1) {
            lv_opa_t opa = (alarm_s1 && !ov->alarm_s1_muted && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            lv_obj_set_style_opa(ov->tank_s1, opa, 0);
        }
        if (ov->tank_r1) {
            lv_opa_t opa = (alarm_r1 && !ov->alarm_r1_muted && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            lv_obj_set_style_opa(ov->tank_r1, opa, 0);
        }

        /* Sonido: 5 segundos cada 5 minutos (300000 ms) mientras la
         * alarma persista y no este silenciada. */
        const uint32_t INTERVAL_MS = 5 * 60 * 1000;
        if (alarm_s1 && !ov->alarm_s1_muted && s_alarm_queue) {
            if (ov->alarm_s1_last_sound_ms == 0 ||
                (now_ms_val - ov->alarm_s1_last_sound_ms) >= INTERVAL_MS) {
                ov->alarm_s1_last_sound_ms = now_ms_val;
                uint8_t v = 1;
                xQueueSend(s_alarm_queue, &v, 0);
            }
        } else {
            /* Reset del timer cuando no hay alarma o cuando se muta */
            ov->alarm_s1_last_sound_ms = 0;
        }
        if (alarm_r1 && !ov->alarm_r1_muted && s_alarm_queue) {
            if (ov->alarm_r1_last_sound_ms == 0 ||
                (now_ms_val - ov->alarm_r1_last_sound_ms) >= INTERVAL_MS) {
                ov->alarm_r1_last_sound_ms = now_ms_val;
                uint8_t v = 1;
                xQueueSend(s_alarm_queue, &v, 0);
            }
        } else {
            ov->alarm_r1_last_sound_ms = 0;
        }

        /* === Alarma SOC < 30 % === */
        bool alarm_soc = ov->bat.has_data && ov->bat.soc_deci < 300;
        if (!alarm_soc && ov->prev_alarm_soc) ov->alarm_soc_muted = false;
        ov->prev_alarm_soc = alarm_soc;
        /* Parpadeo visual del card de bateria */
        if (ov->card_bat) {
            lv_opa_t opa = (alarm_soc && !ov->alarm_soc_muted && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            lv_obj_set_style_opa(ov->card_bat, opa, 0);
        }
        if (alarm_soc && !ov->alarm_soc_muted && s_alarm_queue) {
            if (ov->alarm_soc_last_sound_ms == 0 ||
                (now_ms_val - ov->alarm_soc_last_sound_ms) >= INTERVAL_MS) {
                ov->alarm_soc_last_sound_ms = now_ms_val;
                uint8_t v = 1;
                xQueueSend(s_alarm_queue, &v, 0);
            }
        } else {
            ov->alarm_soc_last_sound_ms = 0;
        }

        /* === Alarma Frigo: T_Congelador > umbral (NVS, default -2 C) === */
        const frigo_state_t *fs_a = frigo_get_state();
        bool alarm_freezer = fs_a && fs_a->T_Congelador > -120.0f
                             && fs_a->T_Congelador > alerts_get_freezer_temp_c();
        if (!alarm_freezer && ov->prev_alarm_freezer) ov->alarm_freezer_muted = false;
        ov->prev_alarm_freezer = alarm_freezer;
        if (alarm_freezer && !ov->alarm_freezer_muted && s_alarm_queue) {
            if (ov->alarm_freezer_last_sound_ms == 0 ||
                (now_ms_val - ov->alarm_freezer_last_sound_ms) >= INTERVAL_MS) {
                ov->alarm_freezer_last_sound_ms = now_ms_val;
                uint8_t v = 1;
                xQueueSend(s_alarm_queue, &v, 0);
            }
        } else {
            ov->alarm_freezer_last_sound_ms = 0;
        }

        /* Indicador 230 V grande */
        if (ov->pill_shore) {
            lv_obj_set_style_bg_color(ov->pill_shore,
                cd.fresh && cd.shore ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM, 0);
        }
        /* El texto "230 V" se mantiene fijo; solo cambia el color del pill */
        /* Estetica del boton se mantiene igual ON/OFF (bg gris, texto
         * del color del acento). Unico indicador de estado: el LED
         * verde con halo cuando ON. */
        #define UPDATE_CAMPER_BTN(btn, on, color_on) do {                   \
            if (!(btn)) break;                                              \
            (void)(color_on);                                               \
            lv_obj_t *led = (lv_obj_t *)lv_obj_get_user_data(btn);          \
            if (led) {                                                      \
                lv_obj_set_style_bg_color(led,                              \
                    (on) ? UI_COLOR_GREEN : lv_color_hex(0x333333), 0);     \
                lv_obj_set_style_shadow_width(led, (on) ? 10 : 0, 0);       \
                lv_obj_set_style_shadow_color(led, UI_COLOR_GREEN, 0);      \
                lv_obj_set_style_shadow_opa(led,                            \
                    (on) ? LV_OPA_80 : LV_OPA_TRANSP, 0);                   \
                lv_obj_set_style_shadow_spread(led, (on) ? 2 : 0, 0);       \
            }                                                               \
        } while (0)

        UPDATE_CAMPER_BTN(ov->btn_lin,  cd.fresh && cd.light_in,  UI_COLOR_YELLOW);
        UPDATE_CAMPER_BTN(ov->btn_lout, cd.fresh && cd.light_out, UI_COLOR_YELLOW);
        UPDATE_CAMPER_BTN(ov->btn_pump, cd.fresh && cd.pump,      UI_COLOR_CYAN);
        #undef UPDATE_CAMPER_BTN
    }

    /* ── Frigo: T_Congelador y % ventilador ─────────────────── */
    {
        const frigo_state_t *fs = frigo_get_state();
        if (fs && ov->lbl_freezer_temp) {
            char fbuf[16];
            float thr = alerts_get_freezer_temp_c();
            bool over = fs->T_Congelador > -120.0f
                        && fs->T_Congelador > thr;
            if (fs->T_Congelador > -120.0f) {
                snprintf(fbuf, sizeof(fbuf), "%.1f \xc2\xb0""C",
                         fs->T_Congelador);
                /* Color: rojo si supera umbral, blanco si OK.
                 * Parpadeo (alfa 30% / 100%) si la alarma esta activa y
                 * sin mutear, para que destaque sobre la barra inferior. */
                lv_obj_set_style_text_color(ov->lbl_freezer_temp,
                    over ? UI_COLOR_RED : UI_COLOR_TEXT, 0);
                lv_opa_t opa = (over && !ov->alarm_freezer_muted
                                && ov->blink_phase)
                    ? LV_OPA_30 : LV_OPA_COVER;
                lv_obj_set_style_text_opa(ov->lbl_freezer_temp, opa, 0);
            } else {
                snprintf(fbuf, sizeof(fbuf), "-- \xc2\xb0""C");
                lv_obj_set_style_text_color(ov->lbl_freezer_temp,
                                            UI_COLOR_TEXT_DIM, 0);
                lv_obj_set_style_text_opa(ov->lbl_freezer_temp,
                                          LV_OPA_COVER, 0);
            }
            lv_label_set_text(ov->lbl_freezer_temp, fbuf);
        }
        if (fs && ov->img_fan) {
            uint8_t p = fs->fan_percent;
            if (p > 100) p = 100;
            /* Interpolacion lineal entre gris (UI_COLOR_TEXT_DIM 0x8A93A6) y
             * cyan (UI_COLOR_CYAN 0x4FC3F7) en funcion del porcentaje. */
            uint8_t r_off = 0x8A, g_off = 0x93, b_off = 0xA6;
            uint8_t r_on  = 0x4F, g_on  = 0xC3, b_on  = 0xF7;
            uint8_t r = r_off + ((int)(r_on - r_off) * p) / 100;
            uint8_t g = g_off + ((int)(g_on - g_off) * p) / 100;
            uint8_t b = b_off + ((int)(b_on - b_off) * p) / 100;
            lv_color_t c = lv_color_make(r, g, b);
            lv_obj_set_style_img_recolor(ov->img_fan, c, 0);
        }
    }

    /* ── Energía solar acumulada del día (kWh hoy en card Solar) ── */
    {
        float pv = energy_today_pv_kwh();
        bool fresh_today = energy_today_is_fresh();
        char ebuf[16];
        if (ov->m_solar_kwh) {
            if (pv > 0.0f || fresh_today) {
                snprintf(ebuf, sizeof(ebuf), "%.2f", pv);
                ui_metric_set(ov->m_solar_kwh, ebuf, "kWh",
                              pv > 0.0f ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
            } else {
                ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);
            }
        }
        /* card_dcdc: m_loads_kwh ya lo gestiona la rama DC/DC arriba con
         * V_out (servicio), no kWh. No tocar aquí. */
    }
}

static void overview_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void overview_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void overview_destroy(ui_device_view_t *view)
{
    if (!view) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}
