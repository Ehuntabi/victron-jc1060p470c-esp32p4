#include "fonts/fonts_es.h"
#include "ausente_mode.h"
#include "esp_log.h"
#include <lvgl.h>
#include "camera.h"
#include "datalogger.h"
#include "nvs.h"

/* Definido en main.c: re-aplica el brillo segun la arbitracion actual
 * (night_mode_timer_cb). Lo llamamos al entrar/salir para efecto inmediato. */
extern void brightness_apply_now(void);
/* Definido en settings_panel.c: sincroniza el switch de "Modo ausente" con el
 * estado real (para que al salir por gesto/HTTP no quede descuadrado). */
extern void settings_ausente_sync_switch(bool on);

static const char *TAG = "ausente";

/* Motivo del ultimo rechazo de ausente_request(true), o NULL si el ultimo
 * intento fue aceptado. Los textos distinguen "sin SD" de "camara no
 * responde"; los usan el dialog del switch (settings_sound.c) y el handler
 * /ausente (portal/config_server.c) para explicar el NO. */
static const char *s_rechazo = NULL;
const char *ausente_rechazo_razon(void) { return s_rechazo; }

typedef enum { AUS_OFF, AUS_PENDING, AUS_ACTIVE } aus_state_t;
static volatile aus_state_t s_state = AUS_OFF;

static lv_timer_t *s_countdown_timer   = NULL;
static lv_obj_t   *s_countdown_overlay = NULL;
static lv_obj_t   *s_countdown_label   = NULL;
static lv_obj_t   *s_guard_overlay     = NULL;  /* negro pantalla completa en modo activo */
static int         s_secs              = 0;

/* Por donde vino la orden que esta en cuenta atras: el cartel no puede decir
 * "apaga el interruptor" a quien la activo desde la app (hallazgo 1.I6). */
static bool s_via_http = false;

/* Aviso de "se reinicio con la vigilancia puesta" (ver ausente_boot_check). */
static const char *s_aviso_reinicio = NULL;
const char *ausente_aviso_reinicio(void) { return s_aviso_reinicio; }

/* ── NVS: "estaba armado" ───────────────────────────────────────────────────
 * El modo vive en RAM: un reinicio (o un corte de corriente) lo apaga y la furgo
 * se queda sin vigilancia. Se guarda un byte para poder avisar en el arranque
 * siguiente, que es lo unico que se puede hacer desde aqui. */
#define NVS_NS_VIG   "vig"
#define NVS_CLAVE_ARM "armado"

static void nvs_armado_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VIG, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, NVS_CLAVE_ARM, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void ausente_boot_check(void)
{
    nvs_handle_t h;
    uint8_t armado = 0;
    if (nvs_open(NVS_NS_VIG, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_u8(h, NVS_CLAVE_ARM, &armado) == ESP_OK && armado) {
        s_aviso_reinicio = "el P4 se reinicio con la vigilancia puesta: "
                           "ahora mismo NO esta vigilando";
        ESP_LOGW(TAG, "%s", s_aviso_reinicio);
    }
    nvs_set_u8(h, NVS_CLAVE_ARM, 0);   /* el aviso es de una sola vez */
    nvs_commit(h);
    nvs_close(h);
}

/* Gesto de salida: 4 toques en CUALQUIERA de las 4 esquinas, en <3 s (el
 * porque de aceptar las cuatro esta en corner_tap_cb, mas abajo). El cartel de
 * la tarjeta "Modo ausente" (ui/settings/settings_sound.c) tiene que decir esto
 * mismo: decia "arriba a la izquierda" hasta el 15-sep-2026. */
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
    nvs_armado_set(1);                    /* para poder avisar si se reinicia estando armado */
    settings_ausente_sync_switch(true);   /* si se armo por HTTP, el switch de Ajustes debe quedar ON */
    clear_countdown();
    create_guard();
    brightness_apply_now();  /* -> night_mode_timer_cb pone brillo 0 (ausente_is_active) */
    camera_set_surveillance(true);   /* movimiento -> foto a /sdcard/vigilancia */
    ESP_LOGI(TAG, "modo ausente ACTIVO (pantalla apagada, vigilancia ON)");
    /* TODO(video): la captura de FOTO ya va; falta arrancar tambien grabacion de
     * video H.264 por evento (ver TODO en camera_stream_task). */
}

/* El cartel de la cuenta atras, segun por donde venga la orden. */
static void countdown_texto(void)
{
    if (!s_countdown_label) return;
    lv_label_set_text_fmt(s_countdown_label, "Modo ausente en %d s\n%s", s_secs,
                          s_via_http ? "(se cancela desde la app o el navegador)"
                                     : "(apaga el interruptor para cancelar)");
}

static void countdown_cb(lv_timer_t *t)
{
    (void)t;
    s_secs--;
    if (s_secs <= 0) {
        activate();
        return;
    }
    countdown_texto();
}

bool ausente_request(bool on)
{
    /* Camino normal: el interruptor de Ajustes. */
    return ausente_request_ex(on, false);
}

bool ausente_request_ex(bool on, bool via_http)
{
    if (on) {
        if (s_state != AUS_OFF) return true;  /* ya pendiente o activo */
        /* Sin SD no hay donde guardar las fotos de vigilancia (ver
         * vig_sd_drain_task/vig_write_jpeg_sd en camera.c): tras "Soltar
         * tarjeta" (trip_manager.c) la SD queda desmontada hasta reiniciar,
         * y armar modo ausente en ese estado activaba igual (log "modo
         * ausente ACTIVO") sin ningun aviso de que cada foto fallaria en
         * silencio -- solo un WARN de camera.c en el log serie, invisible
         * para quien confia en que esto vigila. Detectado por el usuario
         * el 09-sep-2026. */
        if (!datalogger_sd_montada()) {
            ESP_LOGW(TAG, "modo ausente RECHAZADO: SD no montada (soltar "
                          "tarjeta?), no habria donde guardar la vigilancia");
            s_rechazo = "la tarjeta SD no esta montada\n(se solto en Ajustes -> "
                        "Autocaravana), y sin ella la vigilancia no tendria "
                        "donde guardar las fotos.\nReinicia la pantalla para "
                        "volver a montarla.";
            return false;
        }
        /* La SD guarda las fotos, pero si la camara no llego a arrancar
         * (camera_init fallo y main.c lo aisla en silencio) el modo ausente
         * prometeria una vigilancia que no existe. Rechazarlo con aviso
         * claro, igual que sin SD. */
        if (!camera_ready() && !camera_reintentar()) {
            ESP_LOGW(TAG, "modo ausente RECHAZADO: la camara no responde "
                          "(camera_init fallo), no vigilaria nada");
            s_rechazo = "la camara no responde\n(fallo al arrancar): el modo "
                        "ausente no vigilaria nada.\nReinicia la pantalla y "
                        "reintenta.";
            return false;
        }
        s_rechazo = NULL;
        s_state = AUS_PENDING;
        s_secs  = 10;
        s_via_http = via_http;
        /* Si el aviso de reinicio era lo que estaba en pantalla, ya no aplica:
         * se acaba de armar otra vez. */
        s_aviso_reinicio = NULL;

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
        countdown_texto();
        lv_obj_center(s_countdown_label);

        s_countdown_timer = lv_timer_create(countdown_cb, 1000, NULL);
        ESP_LOGI(TAG, "cuenta atras modo ausente: 10 s");
    } else {
        if (s_state == AUS_PENDING) {
            clear_countdown();
            s_state = AUS_OFF;
            /* Igual que al salir del modo activo: si la cancelacion vino de
             * fuera del switch (HTTP /ausente?off), el switch de Ajustes se
             * quedaba en ON mintiendo sobre el estado real. */
            settings_ausente_sync_switch(false);
            ESP_LOGI(TAG, "cuenta atras cancelada");
        } else if (s_state == AUS_ACTIVE) {
            s_state = AUS_OFF;
            nvs_armado_set(0);       /* ya no esta armado: no hay nada que avisar al arrancar */
            destroy_guard();
            brightness_apply_now();  /* restaura el brillo normal */
            camera_set_surveillance(false);   /* parar vigilancia */
            settings_ausente_sync_switch(false);  /* el switch quedaba CHECKED (U1) */
            ESP_LOGI(TAG, "modo ausente DESACTIVADO (gesto/HTTP)");
        }
    }
    return true;
}
