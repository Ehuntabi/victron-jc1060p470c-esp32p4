#include "settings_logs_panel.h"
#include "log_capture/log_capture.h"
#include "ne185/ne185.h"
#include "fonts/fonts_es.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include "esp_log.h"

static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_ne185_counter_label = NULL;
static lv_obj_t *s_ne185_raw_label = NULL;       /* raw bytes live para validacion */
static lv_obj_t *s_verbose_toggle_btn_lbl = NULL;

/* Cuenta log_*.txt en /sdcard. -1 si la SD no se puede abrir.
 * Corre en taskLVGL desde el callback del boton; no se llama en boot. */
static int count_sd_logs(void)
{
    DIR *d = opendir("/sdcard");
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "log_", 4) != 0) continue;
        size_t l = strlen(de->d_name);
        if (l >= 5 && strcmp(de->d_name + l - 4, ".txt") == 0) n++;
    }
    closedir(d);
    return n;
}

/* Refresca el contador SNIFF NE185 mientras la pagina Consola este abierta.
 * Si el firmware NO esta en modo sniffer (counter siempre 0), muestra "OFF". */
static void ne185_counter_refresh_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t raw[20];
    uint32_t n_ok, n_fail;
    ne185_get_last_raw(raw, &n_ok, &n_fail);

    /* Counter de tramas */
    if (s_ne185_counter_label) {
        if (n_ok == 0) {
            lv_label_set_text(s_ne185_counter_label,
                              "NE185 RX: 0 OK  (esperando bus...)");
            lv_obj_set_style_text_color(s_ne185_counter_label,
                                         lv_color_hex(0x999999), 0);
        } else {
            lv_label_set_text_fmt(s_ne185_counter_label,
                                   "NE185 RX: %lu OK  |  %lu fail",
                                   (unsigned long)n_ok, (unsigned long)n_fail);
            lv_obj_set_style_text_color(s_ne185_counter_label,
                                         lv_color_hex(0x8BC34A), 0);
        }
    }

    /* Raw bytes live: solo los relevantes (b5/b6 tank, b12/b13 bat, b15 estado)
     * Util para validar en autocaravana sin sacar SD:
     *  - b15 bit 7 ON cuando enchufas 230V
     *  - b12 cambia cuando enciendes cargador (bat habitaculo sube)
     *  - b13 cambia cuando arranca motor (bat motor)
     *  - b6 cambia si llenas grises (de 0x02 a otro valor) */
    if (s_ne185_raw_label && n_ok > 0) {
        lv_label_set_text_fmt(s_ne185_raw_label,
            "b5=%02X b6=%02X b9=%02X b12=%02X b13=%02X b14=%02X b15=%02X",
            raw[5], raw[6], raw[9], raw[12], raw[13], raw[14], raw[15]);
    }
}

/* Callback de los botones marker (usa el texto del label como marker). */
static void btn_marker_cb(lv_event_t *e)
{
    const char *what = (const char *)lv_event_get_user_data(e);
    if (what) {
        ne185_log_marker(what);
        if (s_status_label) {
            lv_label_set_text_fmt(s_status_label, "MARK: %s", what);
        }
    }
}

static void btn_save_cb(lv_event_t *e)
{
    (void)e;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char path[64];
    snprintf(path, sizeof(path),
             "/sdcard/log_%04d%02d%02d_%02d%02d%02d.txt",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    esp_err_t err = log_capture_save_to_file(path);
    if (s_status_label) {
        if (err == ESP_OK) {
            int total = count_sd_logs();
            if (total >= 0) {
                lv_label_set_text_fmt(s_status_label,
                                       "Guardado:\n%s\nTotal en SD: %d archivos",
                                       path, total);
            } else {
                lv_label_set_text_fmt(s_status_label,
                                       "Guardado:\n%s\n(SD no accesible para conteo)",
                                       path);
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* Lo emite log_capture_save_to_file cuando otro save esta en curso. */
            lv_label_set_text(s_status_label,
                              "Otra escritura en curso, espera unos segundos");
        } else if (count_sd_logs() < 0) {
            /* opendir(/sdcard) fallo -> SD no montada/insertada. */
            lv_label_set_text(s_status_label,
                              LV_SYMBOL_SD_CARD "  Inserta la tarjeta SD");
        } else {
            lv_label_set_text_fmt(s_status_label,
                                  "Error guardando: %s\n"
                                  "(SD insertada pero falla escritura)",
                                  esp_err_to_name(err));
        }
    }
}

/* Toggle VERBOSE log NE185 (hex de cada frame RX al buffer log). */
static void btn_verbose_toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    bool new_state = !ne185_get_verbose();
    ne185_set_verbose(new_state);
    if (s_verbose_toggle_btn_lbl) {
        lv_label_set_text(s_verbose_toggle_btn_lbl,
                          new_state ? LV_SYMBOL_EYE_OPEN "  LOG ON"
                                     : LV_SYMBOL_EYE_CLOSE "  LOG OFF");
    }
    lv_obj_set_style_bg_color(btn,
        new_state ? lv_color_hex(0x00C851)   /* verde activo */
                   : lv_color_hex(0x607D8B), /* gris inactivo */
        0);
}

static lv_obj_t *make_marker_btn(lv_obj_t *parent, const char *text,
                                  lv_color_t bg, const char *marker_str)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 180, 50);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, btn_marker_cb, LV_EVENT_CLICKED,
                         (void *)marker_str);
    return btn;
}

void settings_logs_panel_create(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;

    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    lv_obj_t *info = lv_label_create(cont);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(info, "Vuelca el buffer de logs en RAM a la SD\n"
                              "(archivo log_YYYYMMDD_HHMMSS.txt)");

    lv_obj_t *btn = lv_btn_create(cont);
    lv_obj_set_size(btn, 260, 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_SAVE "  Guardar a SD");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn, btn_save_cb, LV_EVENT_CLICKED, NULL);

    /* Botón toggle VERBOSE log NE185: OFF (gris) <-> ON (verde).
     * Cuando ON, cada frame RX se loguea con hex de bytes desconocidos. */
    lv_obj_t *btn_verbose = lv_btn_create(cont);
    lv_obj_set_size(btn_verbose, 260, 60);
    lv_obj_set_style_bg_color(btn_verbose, lv_color_hex(0x607D8B), 0);
    lv_obj_set_style_radius(btn_verbose, 12, 0);
    s_verbose_toggle_btn_lbl = lv_label_create(btn_verbose);
    lv_label_set_text(s_verbose_toggle_btn_lbl, LV_SYMBOL_EYE_CLOSE "  LOG OFF");
    lv_obj_set_style_text_font(s_verbose_toggle_btn_lbl,
                                &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_verbose_toggle_btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_verbose_toggle_btn_lbl);
    lv_obj_add_event_cb(btn_verbose, btn_verbose_toggle_cb, LV_EVENT_CLICKED, NULL);

    s_status_label = lv_label_create(cont);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(s_status_label, "");

    /* Contador SNIFF NE185 - visible cuando hay tramas, indica si el bus
     * responde sin necesidad de abrir el log SD. */
    s_ne185_counter_label = lv_label_create(cont);
    lv_obj_set_style_text_font(s_ne185_counter_label,
                                &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_ne185_counter_label,
                                 lv_color_hex(0x999999), 0);
    lv_label_set_text(s_ne185_counter_label,
                      "NE185 RX: 0 OK  (esperando bus...)");

    /* Label de bytes RAW live (validacion in-situ sin SD).
     * Mira b15 bit 7 para shore, b12 para bateria habit, b13 motor, b6 grises. */
    s_ne185_raw_label = lv_label_create(cont);
    lv_obj_set_style_text_font(s_ne185_raw_label,
                                &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_ne185_raw_label,
                                 lv_color_hex(0xFFC107), 0);  /* ambar */
    lv_label_set_text(s_ne185_raw_label,
                      "b5=- b6=- b9=- b12=- b13=- b14=- b15=-");

    /* Timer cada 500ms para refrescar contador + raw */
    lv_timer_create(ne185_counter_refresh_cb, 500, NULL);

    /* 4 botones marker para etiquetar el log durante pruebas in-situ */
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 8, 0);
    make_marker_btn(row, "230V ON",     lv_color_hex(0x4CAF50), "230V ON");
    make_marker_btn(row, "230V OFF",    lv_color_hex(0x9E9E9E), "230V OFF");
    make_marker_btn(row, "Cargador ON", lv_color_hex(0x2196F3), "Cargador ON");
    make_marker_btn(row, "Cargador OFF",lv_color_hex(0x607D8B), "Cargador OFF");
}
