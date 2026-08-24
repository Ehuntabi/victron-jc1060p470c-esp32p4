/* Pagina "Sonido y alertas" de Ajustes: volumen/silenciar, umbrales de SoC y
 * de congelador, y la tarjeta de Modo ausente (vive aqui porque su switch
 * comparte estetica y card-style con el resto de esta pagina).
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver
 * settings_common.h). Es el mismo codigo movido de sitio, sin cambios de
 * comportamiento.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "ui/vigilancia/ausente_mode.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "alerts.h"

#include <stddef.h>
#include <lvgl.h>

/* === SoC umbrales (dropdowns) === */
static const int s_soc_crit_options[] = { 10, 20, 30, 40 };
static const int s_soc_warn_options[] = { 40, 50, 60, 70 };

static void soc_crit_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0])) {
        alerts_set_soc_critical(s_soc_crit_options[sel]);
    }
}

static void soc_warn_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0])) {
        alerts_set_soc_warning(s_soc_warn_options[sel]);
    }
}

static void alarm_min_dd_cb_sound(lv_event_t *e);
static void alarm_temp_dd_cb_sound(lv_event_t *e);

/* Refs para que el switch 'Silenciar avisos' maneje el slider de volumen:
 * ON -> guarda el volumen actual y lo pone a 0; OFF -> retoma el guardado. */
static lv_obj_t *s_vol_slider  = NULL;
static lv_obj_t *s_vol_label   = NULL;
static lv_obj_t *s_mute_switch = NULL;
static int       s_vol_saved   = 50;

static void sound_volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    /* Redondeo a multiplos de 5 */
    v = (v / 5) * 5;
    lv_slider_set_value(slider, v, LV_ANIM_OFF);
    audio_set_volume(v);
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "Volumen: %d%%", v);
    /* Si suben el volumen con el silencio activo, se desactiva el silencio. */
    if (v > 0 && s_mute_switch && lv_obj_is_valid(s_mute_switch) &&
        lv_obj_has_state(s_mute_switch, LV_STATE_CHECKED)) {
        lv_obj_clear_state(s_mute_switch, LV_STATE_CHECKED);
        audio_set_mute(false);
    }
}

/* Aplica mute/unmute con guardado y restauracion del volumen, y sincroniza los
 * widgets de Settings (slider, etiqueta y switch) si ya existen. La llaman el
 * switch "Silenciar avisos" y el icono del altavoz de la barra inferior, para
 * que el comportamiento sea identico desde ambos sitios. */
void ui_settings_apply_mute(bool muted)
{
    audio_set_mute(muted);
    if (muted) {
        s_vol_saved = audio_get_volume();   /* recordar antes de poner a 0 */
        audio_set_volume(0);
    } else {
        audio_set_volume(s_vol_saved);      /* retomar el ultimo valor guardado */
    }
    int v = audio_get_volume();
    if (s_vol_slider && lv_obj_is_valid(s_vol_slider))
        lv_slider_set_value(s_vol_slider, v, LV_ANIM_OFF);
    if (s_vol_label && lv_obj_is_valid(s_vol_label))
        lv_label_set_text_fmt(s_vol_label, "Volumen: %d%%", v);
    if (s_mute_switch && lv_obj_is_valid(s_mute_switch)) {
        if (muted) lv_obj_add_state(s_mute_switch, LV_STATE_CHECKED);
        else       lv_obj_clear_state(s_mute_switch, LV_STATE_CHECKED);
    }
}

static void sound_mute_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ui_settings_apply_mute(lv_obj_has_state(sw, LV_STATE_CHECKED));
}


/* Puntero al switch de ausente para poder sincronizarlo cuando se sale del modo
 * por el gesto de 4 toques (si no, el switch queda CHECKED y hay que pulsarlo dos
 * veces para re-armar). La pagina de Settings se cachea, asi que persiste. */
static lv_obj_t *s_ausente_sw = NULL;

/* Sincroniza el switch con el estado real del modo ausente. La llama ausente_mode
 * al entrar/salir. Debe ejecutarse en la tarea LVGL (lo garantizan sus llamadores:
 * el gesto corre en LVGL; la salida por HTTP toma lvgl_port_lock). */
void settings_ausente_sync_switch(bool on)
{
    if (!s_ausente_sw) return;
    if (on) lv_obj_add_state(s_ausente_sw, LV_STATE_CHECKED);
    else    lv_obj_clear_state(s_ausente_sw, LV_STATE_CHECKED);
}

/* Switch del modo ausente/vigilancia: al activar, ausente_request inicia la
 * cuenta atras de 10 s; al desactivar, cancela (la salida real del modo activo
 * es con 4 toques en la esquina, no por este switch). */
static void ausente_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ausente_request(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Card "Modo ausente / vigilancia", reubicada al submenu Autocaravana (antes
 * estaba en la pagina "Sonido y alertas"). */
void create_ausente_card(lv_obj_t *cont)
{
    lv_obj_t *card_aus = lv_obj_create(cont);
    lv_obj_set_width(card_aus, lv_pct(100));
    lv_obj_set_height(card_aus, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_aus, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_aus, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_aus, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(card_aus, 1, 0);
    lv_obj_set_style_radius(card_aus, 12, 0);
    /* Relleno vertical 8 y separacion 4, IGUAL que las tarjetas vecinas
     * (Energia del viaje, Bombonas). Esta usaba 16 y 8, o sea que era la mas
     * alta de la pagina sin motivo. Entre esto y el texto de una sola linea se
     * recuperan unos 48 px, que es lo que hacia falta para que las cuatro
     * tarjetas de Autocaravana quepan en pantalla (24-ago-2026). El relleno
     * horizontal se queda en 16, que ese no estorba. */
    lv_obj_set_style_pad_hor(card_aus, 16, 0);
    lv_obj_set_style_pad_ver(card_aus, 8, 0);
    lv_obj_set_style_pad_gap(card_aus, 4, 0);
    lv_obj_set_layout(card_aus, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_aus, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *aus_row = lv_obj_create(card_aus);
    lv_obj_remove_style_all(aus_row);
    lv_obj_set_size(aus_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(aus_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(aus_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(aus_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *aus_title = lv_label_create(aus_row);
    lv_obj_set_style_text_font(aus_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(aus_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(aus_title, LV_SYMBOL_EYE_OPEN "  Modo ausente");

    lv_obj_t *aus_sw = lv_switch_create(aus_row);
    lv_obj_set_style_bg_color(aus_sw, lv_color_hex(0x4FC3F7), LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(aus_sw, ausente_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ausente_sw = aus_sw;   /* para sincronizarlo al salir por gesto (U1) */

    lv_obj_t *aus_hint = lv_label_create(card_aus);
    lv_obj_set_style_text_font(aus_hint, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(aus_hint, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_width(aus_hint, lv_pct(100));
    /* UNA sola linea (peticion del usuario, 24-ago-2026). Antes eran dos, con
      * un salto de linea a mano. El texto se acorto para que quepa: la tarjeta
      * ocupa el ancho entero (~960 px utiles) y a font 20 eso son unos 95
      * caracteres; los dos renglones de antes sumaban 120. LONG_DOT y no WRAP
      * para que, si algun dia crece, se corte en vez de volver a partirse en
      * dos -- que es justo lo que se venia a quitar. */
    lv_label_set_long_mode(aus_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(aus_hint,
                      "Apaga la pantalla y vigila (arranca en 10 s). "
                      "Salir: 4 toques arriba a la izquierda.");
}

void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;
    style_settings_scrollbar(page);
    /* Contenedor principal vertical */
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Sonido === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(100));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(0xFF7043), 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_radius(card1, 12, 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_set_style_pad_gap(card1, 12, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Fila titulo: 'Sonido' (izda) + 'Volumen: X%' (dcha, sobre el slider) */
    lv_obj_t *title_row = lv_obj_create(card1);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(title_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card1_title = lv_label_create(title_row);
    /* Montserrat built-in para que el LV_SYMBOL_VOLUME_MAX se renderice. */
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0xFF7043), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_VOLUME_MAX "  Sonido");

    lv_obj_t *lbl_vol = lv_label_create(title_row);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20_es, 0);
    lv_label_set_text_fmt(lbl_vol, "Volumen: %d%%", audio_get_volume());

    /* Fila: silenciar avisos (izda, texto+switch) + slider de volumen (dcha) */
    lv_obj_t *ctl_row = lv_obj_create(card1);
    lv_obj_remove_style_all(ctl_row);
    lv_obj_set_size(ctl_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(ctl_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(ctl_row, 10, 0);     /* separa un poco 'Silenciar avisos' */
    lv_obj_set_style_pad_column(ctl_row, 14, 0);   /* hueco entre el texto y el switch */

    /* Silenciar avisos a la IZQUIERDA (texto + switch) */
    lv_obj_t *lbl_mute = lv_label_create(ctl_row);
    lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_mute, "Silenciar avisos");

    lv_obj_t *sw = lv_switch_create(ctl_row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFF7043), LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (audio_is_muted()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sound_mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui->sound_mute_switch = sw;

    /* Espaciador flexible: empuja el slider al borde derecho */
    lv_obj_t *spacer = lv_obj_create(ctl_row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    /* Slider de volumen a la DERECHA */
    lv_obj_t *slider = lv_slider_create(ctl_row);
    lv_obj_set_width(slider, 440);
    lv_obj_set_height(slider, 26);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF7043), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF7043), LV_PART_KNOB);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, sound_volume_changed_cb, LV_EVENT_VALUE_CHANGED, lbl_vol);

    /* Refs para que 'Silenciar avisos' maneje el slider (guardar/0/restaurar). */
    s_vol_slider  = slider;
    s_vol_label   = lbl_vol;
    s_mute_switch = sw;
    if (!audio_is_muted()) s_vol_saved = audio_get_volume();

    /* (La card "Modo ausente / vigilancia" se movio al submenu Autocaravana:
     *  ver create_ausente_card / populate_autocaravana.) */

    /* === Card 2: Bateria === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 16, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_BATTERY_FULL "  Bateria");

    /* row_soc deja de ser child de card2; usa card2 como flex padre directamente */
    lv_obj_t *row_soc = card2;

    /* SoC Critico */
    lv_obj_t *col_crit = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_crit);
    lv_obj_set_layout(col_crit, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_crit, LV_FLEX_FLOW_ROW);   /* icono+texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_crit, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_crit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_crit, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_crit = lv_label_create(col_crit);
    lv_obj_set_style_text_font(lbl_crit, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_crit, lv_color_hex(0xFF4444), 0);
    lv_label_set_text(lbl_crit, LV_SYMBOL_WARNING " Critico");
    lv_obj_t *dd_crit = lv_dropdown_create(col_crit);
    lv_obj_set_width(dd_crit, 130);
    lv_obj_set_style_text_font(dd_crit, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_crit, "10 %\n20 %\n30 %\n40 %");
    {
        int cur = alerts_get_soc_critical();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0]); ++k) {
            if (s_soc_crit_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_crit, idx);
    }
    lv_obj_add_event_cb(dd_crit, soc_crit_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* SoC Aviso */
    lv_obj_t *col_warn = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_warn);
    lv_obj_set_layout(col_warn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_warn, LV_FLEX_FLOW_ROW);   /* icono+texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_warn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_warn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_warn, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_warn = lv_label_create(col_warn);
    lv_obj_set_style_text_font(lbl_warn, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_warn, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(lbl_warn, LV_SYMBOL_BELL " Aviso");
    lv_obj_t *dd_warn = lv_dropdown_create(col_warn);
    lv_obj_set_width(dd_warn, 130);
    lv_obj_set_style_text_font(dd_warn, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_warn, "40 %\n50 %\n60 %\n70 %");
    {
        int cur = alerts_get_soc_warning();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0]); ++k) {
            if (s_soc_warn_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_warn, idx);
    }
    lv_obj_add_event_cb(dd_warn, soc_warn_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* === Card 3: Congelador === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 16, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_CHARGE "  Congelador");

    lv_obj_t *row_frigo = card3;

    /* Col minutos */
    lv_obj_t *col_min_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_min_a);
    lv_obj_set_layout(col_min_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_min_a, LV_FLEX_FLOW_ROW);   /* texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_min_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_min_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_min_a, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_min_a = lv_label_create(col_min_a);
    lv_obj_set_style_text_font(lbl_min_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_min_a, "Tras subir (min)");
    lv_obj_t *dd_min_a = lv_dropdown_create(col_min_a);
    lv_obj_set_width(dd_min_a, 130);
    lv_obj_set_style_text_font(dd_min_a, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_min_a, "15\n30\n45\n60\n90");
    {
        static const int opts[] = { 15, 30, 45, 60, 90 };
        int cur = alerts_get_freezer_minutes();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_min_a, idx);
    }
    lv_obj_add_event_cb(dd_min_a, alarm_min_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);

    /* Col temp umbral */
    lv_obj_t *col_t_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_t_a);
    lv_obj_set_layout(col_t_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_t_a, LV_FLEX_FLOW_ROW);   /* texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_t_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_t_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_t_a, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_t_a = lv_label_create(col_t_a);
    lv_obj_set_style_text_font(lbl_t_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_t_a, "Si supera");
    lv_obj_t *dd_t_a = lv_dropdown_create(col_t_a);
    lv_obj_set_width(dd_t_a, 140);
    lv_obj_set_style_text_font(dd_t_a, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_t_a, "-5 \xc2\xb0""C\n-2 \xc2\xb0""C\n0 \xc2\xb0""C\n+2 \xc2\xb0""C");
    {
        static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
        float cur = alerts_get_freezer_temp_c();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_t_a, idx);
    }
    lv_obj_add_event_cb(dd_t_a, alarm_temp_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);
}


static void alarm_min_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const int opts[] = { 15, 30, 45, 60, 90 };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_minutes(opts[sel]);
}

static void alarm_temp_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_temp_c(opts[sel]);
}
