#include "ausente_mode.h"
#include "esp_log.h"
#include <lvgl.h>
#include "camera.h"

/* Definido en main.c: re-aplica el brillo segun la arbitracion actual
 * (night_mode_timer_cb). Lo llamamos al entrar/salir para efecto inmediato. */
extern void brightness_apply_now(void);
/* Definido en settings_panel.c: sincroniza el switch de "Modo ausente" con el
 * estado real (para que al salir por gesto/HTTP no quede descuadrado). */
extern void settings_ausente_sync_switch(bool on);

static const char *TAG = "ausente";

typedef enum { AUS_OFF, AUS_PENDING, AUS_ACTIVE } aus_state_t;
static aus_state_t s_state = AUS_OFF;

static lv_timer_t *s_countdown_timer   = NULL;
static lv_obj_t   *s_countdown_overlay = NULL;
static lv_obj_t   *s_countdown_label   = NULL;
static lv_obj_t   *s_guard_overlay     = NULL;  /* negro pantalla completa en modo activo */
static int         s_secs              = 0;

/* Gesto de salida: 4 toques en la esquina superior izquierda en <3 s. */
#define CORNER_PX      130
#define TAP_WINDOW_MS  3000
#define TAP_COUNT      4
static int      s_taps        = 0;
static uint32_t s_first_tap_ms = 0;

bool ausente_is_active(void) { return s_state == AUS_ACTIVE; }

static void clear_countdown(void)
{
    if (s_countdown_timer)   { lv_timer_del(s_countdown_timer);   s_countdown_timer = NULL; }
    if (s_countdown_overlay) { lv_obj_del(s_countdown_overlay);   s_countdown_overlay = NULL; s_countdown_label = NULL; }
}

/* Overlay negro a pantalla completa que se come los toques (para que la UI no
 * reaccione con la pantalla apagada) y cuenta los 4 toques de la esquina. */
static void guard_clicked_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* Aceptar CUALQUIERA de las 4 esquinas (no solo la sup-izq): si el tactil tiene
     * una zona muerta en una esquina, sigue habiendo salida -> evita quedar atrapado
     * con la pantalla negra. El guard es pantalla completa, de el sacamos W y H. */
    lv_coord_t W = lv_obj_get_width(s_guard_overlay);
    lv_coord_t H = lv_obj_get_height(s_guard_overlay);
    bool in_corner = (p.x < CORNER_PX || p.x > W - CORNER_PX) &&
                     (p.y < CORNER_PX || p.y > H - CORNER_PX);
    if (!in_corner) {
        return;  /* fuera de las esquinas: toque comido, no hace nada */
    }
    uint32_t now = lv_tick_get();
    if (s_taps == 0 || (now - s_first_tap_ms) > TAP_WINDOW_MS) {
        s_taps = 1;
        s_first_tap_ms = now;
    } else {
        s_taps++;
    }
    ESP_LOGI(TAG, "toque esquina %d/%d", s_taps, TAP_COUNT);
    if (s_taps >= TAP_COUNT) {
        s_taps = 0;
        ausente_request(false);  /* salir */
    }
}

static void create_guard(void)
{
    if (s_guard_overlay) return;
    s_guard_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_guard_overlay);
    lv_obj_set_size(s_guard_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_guard_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_guard_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_guard_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_guard_overlay, guard_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_taps = 0;
}

static void destroy_guard(void)
{
    if (s_guard_overlay) { lv_obj_del(s_guard_overlay); s_guard_overlay = NULL; }
}

static void activate(void)
{
    s_state = AUS_ACTIVE;
    clear_countdown();
    create_guard();
    brightness_apply_now();  /* -> night_mode_timer_cb pone brillo 0 (ausente_is_active) */
    camera_set_surveillance(true);   /* movimiento -> foto a /sdcard/vigilancia */
    ESP_LOGI(TAG, "modo ausente ACTIVO (pantalla apagada, vigilancia ON)");
    /* TODO(video): la captura de FOTO ya va; falta arrancar tambien grabacion de
     * video H.264 por evento (ver TODO en camera_stream_task). */
}

static void countdown_cb(lv_timer_t *t)
{
    (void)t;
    s_secs--;
    if (s_secs <= 0) {
        activate();
        return;
    }
    if (s_countdown_label) {
        lv_label_set_text_fmt(s_countdown_label,
                              "Modo ausente en %d s\n(apaga el switch para cancelar)", s_secs);
    }
}

void ausente_request(bool on)
{
    if (on) {
        if (s_state != AUS_OFF) return;  /* ya pendiente o activo */
        s_state = AUS_PENDING;
        s_secs  = 10;

        /* Overlay semitransparente NO clickable: muestra la cuenta atras pero
         * deja pasar los toques al switch de abajo (para poder cancelar). */
        s_countdown_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_countdown_overlay);
        lv_obj_set_size(s_countdown_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_countdown_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_countdown_overlay, LV_OPA_70, 0);
        lv_obj_clear_flag(s_countdown_overlay, LV_OBJ_FLAG_CLICKABLE);

        s_countdown_label = lv_label_create(s_countdown_overlay);
        lv_obj_set_style_text_color(s_countdown_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_countdown_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(s_countdown_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(s_countdown_label,
                              "Modo ausente en %d s\n(apaga el switch para cancelar)", s_secs);
        lv_obj_center(s_countdown_label);

        s_countdown_timer = lv_timer_create(countdown_cb, 1000, NULL);
        ESP_LOGI(TAG, "cuenta atras modo ausente: 10 s");
    } else {
        if (s_state == AUS_PENDING) {
            clear_countdown();
            s_state = AUS_OFF;
            ESP_LOGI(TAG, "cuenta atras cancelada");
        } else if (s_state == AUS_ACTIVE) {
            s_state = AUS_OFF;
            destroy_guard();
            brightness_apply_now();  /* restaura el brillo normal */
            camera_set_surveillance(false);   /* parar vigilancia */
            settings_ausente_sync_switch(false);  /* el switch quedaba CHECKED (U1) */
            ESP_LOGI(TAG, "modo ausente DESACTIVADO (gesto/HTTP)");
        }
    }
}
