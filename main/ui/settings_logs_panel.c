#include "settings_logs_panel.h"
#include "log_capture/log_capture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"

static lv_obj_t *s_status_label = NULL;

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
            lv_label_set_text_fmt(s_status_label, "Guardado:\n%s", path);
        } else {
            lv_label_set_text_fmt(s_status_label, "Error: %s",
                                  esp_err_to_name(err));
        }
    }
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
    lv_obj_set_style_text_color(info, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(info, "Vuelca el buffer de logs en RAM a la SD\n"
                              "(archivo log_YYYYMMDD_HHMMSS.txt)");

    lv_obj_t *btn = lv_btn_create(cont);
    lv_obj_set_size(btn, 260, 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_SAVE "  Guardar a SD");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn, btn_save_cb, LV_EVENT_CLICKED, NULL);

    s_status_label = lv_label_create(cont);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(s_status_label, "");
}
