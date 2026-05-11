#include "splash.h"
#include "icons/icons.h"
#include "fonts/fonts_es.h"
#include "config_storage.h"
#include "esp_log.h"

static const char *TAG = "SPLASH";
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_bar = NULL;
static lv_timer_t *s_progress_timer = NULL;
static int s_progress = 0;

static void progress_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_bar) return;
    s_progress += 5;
    if (s_progress > 100) s_progress = 100;
    lv_bar_set_value(s_bar, s_progress, LV_ANIM_ON);
    if (s_progress >= 100) {
        if (s_progress_timer) {
            lv_timer_del(s_progress_timer);
            s_progress_timer = NULL;
        }
    }
}

bool splash_show(void)
{
    uint8_t mode = 1;
    load_splash_mode(&mode);
    if (mode == 0) {
        ESP_LOGI(TAG, "Splash deshabilitado por NVS");
        return false;
    }

    s_screen = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_screen, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x06080C), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_screen, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_screen, 20, 0);

    /* Logo (480x256) */
    lv_obj_t *img = lv_img_create(s_screen);
    lv_img_set_src(img, &splash_logo);

    /* Texto principal */
    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(title, "VictronSolarDisplay");

    /* Sub-texto */
    lv_obj_t *sub = lv_label_create(s_screen);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(sub, "Iniciando...");

    /* Barra de progreso */
    s_bar = lv_bar_create(s_screen);
    lv_obj_set_size(s_bar, 320, 12);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2D3340), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFF9800), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_INDICATOR);

    s_progress = 0;
    s_progress_timer = lv_timer_create(progress_timer_cb, 80, NULL);

    ESP_LOGI(TAG, "Splash mostrado");
    return true;
}

void splash_hide(void)
{
    if (s_progress_timer) {
        lv_timer_del(s_progress_timer);
        s_progress_timer = NULL;
    }
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
        s_bar = NULL;
        ESP_LOGI(TAG, "Splash oculto");
    }
}
