/* ui.c */
#include "ui.h"
#include "gps/gps.h"
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
#include "victron_products.h"
#include "ui/views/frigo_panel.h"
#include "ui/vigilancia/gallery.h"
#include "ui/devices/device_tracker.h"
#include "ui/vigilancia/capture_carousel.h"
#include "ui/devices/ble_ingest.h"
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

static int64_t s_last_ble_data_us = 0;
static void ble_indicator_timer_cb(lv_timer_t *t);
static void gps_indicator_timer_cb(lv_timer_t *t);

/* Estado de 'wifi/enabled' cacheado en RAM para el icono de la barra. -1 = aun
 * sin leer. Ver el porque de la cache (y de por que hay que avisarla a mano) en
 * el refresco del icono, mas abajo. */
static int s_wifi_enabled_cache = -1;

void ui_wifi_set_enabled_cache(bool enabled)
{
    s_wifi_enabled_cache = enabled ? 1 : 0;
}


// Font Awesome symbols (declared in main.c)
LV_FONT_DECLARE(font_awesome_solar_panel_40);
LV_FONT_DECLARE(font_awesome_bolt_40);

static const char *TAG_UI = "UI_MODULE";

static ui_state_t g_ui = {
    .brightness = 100,
    .current_device_type = VICTRON_BLE_RECORD_TEST,
    .current_product_id = 0,
    .has_received_data = false,
    .tab_settings_index = UINT16_MAX,
    .default_view = NULL,
    .victron_config = {0},
    .current_device_mac = "00:00:00:00:00:00",
    .view_selection = { .mode = UI_VIEW_MODE_DEFAULT_BATTERY, .dropdown = NULL },
    .lbl_device_type = NULL,
    .lbl_product_name = NULL,
    .lbl_error = NULL,
    .ta_mac = NULL,
    .ta_key = NULL,
};

// Forward declarations
static void tabview_touch_event_cb(lv_event_t *e);

/* Tarea aparte para los beeps de click. Asi el handler del click NO bloquea
 * el thread LVGL durante los ~300 ms que tarda audio_play_jingle (que entre
 * tono + padding de silencio del codec bloquea el llamante). Si el usuario
 * pulsa varios botones rapido, los beeps adicionales se descartan (cola de
 * tamaño 1 con xQueueOverwrite no es necesario — basta xQueueSend con
 * timeout 0 que devuelve fail si la cola esta llena). */
static QueueHandle_t s_beep_queue = NULL;

static void ui_beep_task(void *arg)
{
    (void)arg;
    uint8_t jingle;
    while (1) {
        if (xQueueReceive(s_beep_queue, &jingle, portMAX_DELAY) == pdTRUE) {
            audio_play_jingle((audio_jingle_t)jingle);
        }
    }
}

/* Encola un jingle para que suene en ui_beep_task, NUNCA en el sitio del
 * llamante (que puede tener el lock LVGL cogido y/o correr en la task NimBLE).
 * priority=true (alarmas) usa xQueueOverwrite: reemplaza un beep de click
 * pendiente para no perderse. priority=false (clicks) se descarta si llena. */
void ui_enqueue_jingle(audio_jingle_t j, bool priority)
{
    if (!s_beep_queue) return;
    uint8_t v = (uint8_t)j;
    if (priority) xQueueOverwrite(s_beep_queue, &v);
    else          xQueueSend(s_beep_queue, &v, 0);
}

/* Beep corto al pulsar cualquier widget clicable. Encola y vuelve
 * inmediatamente (no bloquea el thread LVGL). No-static: tambien la usa
 * battery_history_screen.c (declaracion extern ahi). */
void ui_global_click_beep_cb(lv_event_t *e)
{
    (void)e;
    /* Se descarta si la cola esta llena (beep en curso): evita acumulacion
     * de beeps por clicks rapidos. */
    ui_enqueue_jingle(AUDIO_JINGLE_CONFIRM, false);
}

static void solar_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_solar) return;
    if (frigo_solar_get_active())
        lv_obj_clear_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);
}

static void volume_icon_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_volume) return;
    static bool last_muted = false;
    bool muted = audio_is_muted();
    if (muted != last_muted) {
        lv_label_set_text(ui->lbl_volume, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(ui->lbl_volume,
            muted ? lv_color_hex(0xFF4444) : lv_color_white(), 0);
        last_muted = muted;
    }
    /* Refresca wifi icon. Leer NVS solo UNA vez y cachear en RAM: hacerlo en cada
     * tick (cada 500ms) provocaba INT WDT -> nvs_get_u8 -> esp_partition_read ->
     * spi_flash_disable_interrupts_caches_and_other_cpu apaga la cache de flash y
     * para el otro core; a ~2/seg, tarde o temprano la ventana coincidia con CPU1
     * ocupado (GDMA camara/esp_hosted) y pasaba de 300ms.
     *
     * La cache NO se puede refrescar sola: quien cambia 'wifi/enabled' avisa por
     * ui_wifi_set_enabled_cache(). Antes bastaba con leerlo una vez al arrancar,
     * porque cualquier cambio del flag reiniciaba la placa (pasaba siempre por el
     * dialogo de reinicio -> esp_restart). Desde que el toggle se aplica EN
     * CALIENTE eso ya no es cierto y sin el aviso el icono se quedaria con el
     * color del arranque hasta el siguiente reinicio. */
    if (ui->lbl_wifi) {
        static int last_en = -1;
        static int last_portal = -1;
        if (s_wifi_enabled_cache < 0) {
            nvs_handle_t h;
            uint8_t en = 1;
            if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
                nvs_get_u8(h, "enabled", &en);
                nvs_close(h);
            }
            s_wifi_enabled_cache = en;
        }
        const int cached_en = s_wifi_enabled_cache;
        /* config_server_is_running() solo mira un puntero (no toca flash), asi que
         * es seguro sondearlo cada tick. El portal puede apagarse solo (auto-off),
         * por eso el color se refresca segun su estado real, no solo al arrancar. */
        int portal = cached_en ? (config_server_is_running() ? 1 : 0) : 0;
        if (cached_en != last_en || portal != last_portal) {
            /* Verde: portal web accesible. Azul: WiFi encendido pero portal
             * apagado. Gris: WiFi deshabilitado. */
            uint32_t color = !cached_en ? 0x666666
                           : portal     ? 0x00C851
                                        : 0x4FC3F7;
            lv_obj_set_style_text_color(ui->lbl_wifi, lv_color_hex(color), 0);
            last_en = cached_en;
            last_portal = portal;
        }
    }
}
static const char *device_type_name(victron_record_type_t type);
static void clock_timer_cb(lv_timer_t *timer);
static void idle_to_live_timer_cb(lv_timer_t *t);
static void nav_btn_event_cb(lv_event_t *e);
static void settings_auto_return_cb(lv_timer_t *t);
static void nav_icon_sync_cb(lv_event_t *e);
static void volume_btn_event_cb(lv_event_t *e);
static void wifi_btn_event_cb(lv_event_t *e);
static lv_timer_t *s_idle_to_live_timer;

/* Overlay de detalle de una card del Overview (Solar/Bateria/DC-DC): se abre al
 * pulsar la card y se cierra con el boton volver o tras 1 min sin tocar. */
static bool      s_card_detail_active   = false;
static lv_obj_t *s_card_detail_back_btn = NULL;
/* Ultimo record recibido por fuente, para pintar el detalle al instante. */
static victron_data_t s_last_solar;   static bool s_has_solar   = false;
static victron_data_t s_last_battery; static bool s_has_battery = false;
static victron_data_t s_last_dcdc;    static bool s_has_dcdc    = false;

#define IDLE_TO_LIVE_TIMEOUT_MS 60000

static bool obj_is_descendant(const lv_obj_t *obj, const lv_obj_t *parent)
{
    if (obj == NULL || parent == NULL) {
        return false;
    }
    const lv_obj_t *current = obj;
    while (current != NULL) {
        if (current == parent) {
            return true;
        }
        current = lv_obj_get_parent(current);
    }
    return false;
}

void ui_init(void) {
    ui_state_t *ui = &g_ui;

    load_brightness(&ui->brightness);
    load_night_mode(&ui->night_mode.enabled,
                    &ui->night_mode.start_h,
                    &ui->night_mode.end_h);

    ui->active_view = NULL;
    ui->default_view = NULL;
    ui->current_device_type = VICTRON_BLE_RECORD_TEST;
    strcpy(ui->current_device_mac, "00:00:00:00:00:00");
    ui->ta_mac = NULL;          // Legacy field - no longer created in System page
    ui->ta_key = NULL;          // Legacy field - no longer created in System page  
    ui->lbl_device_type = NULL; // Legacy field - no longer created in System page
    ui->lbl_product_name = NULL;// Legacy field - no longer created in System page
    ui->lbl_error = NULL;       // Legacy field - no longer created in System page
    for (size_t i = 0; i < UI_MAX_DEVICE_VIEWS; ++i) {
        ui->views[i] = NULL;
    }


    ui->victron_config.count = 0;
    ui->victron_config.container = NULL;
    ui->victron_config.list = NULL;
    ui->victron_config.add_btn = NULL;
    ui->victron_config.remove_btn = NULL;
    ui->victron_config.updating = false;
    for (size_t i = 0; i < UI_MAX_VICTRON_DEVICES; ++i) {
        ui->victron_config.rows[i] = NULL;
        ui->victron_config.mac_textareas[i] = NULL;
        ui->victron_config.key_textareas[i] = NULL;
        ui->victron_config.name_textareas[i] = NULL;
        ui->victron_config.enabled_checkboxes[i] = NULL;
        ui->victron_config.device_type_labels[i] = NULL;
        ui->victron_config.product_name_labels[i] = NULL;
        ui->victron_config.error_labels[i] = NULL;
        ui->victron_config.status_containers[i] = NULL;
        
        // Initialize device activity tracking
        ui->last_active_devices[i][0] = '\0';
        ui->last_activity_time[i] = 0;
    }
    
    // Timer que marca "Offline" a los dispositivos sin actividad reciente
    device_tracker_init(ui);

    /* Initialize view selection - load saved mode or use Overview por defecto */
    uint8_t saved_mode = (uint8_t)UI_VIEW_MODE_OVERVIEW;
    if (load_ui_view_mode(&saved_mode) == ESP_OK) {
        ui->view_selection.mode = (ui_view_mode_t)saved_mode;
    } else {
        ui->view_selection.mode = UI_VIEW_MODE_OVERVIEW;
    }
    ui->view_selection.dropdown = NULL;


    char default_ssid[33]; size_t ssid_len = sizeof(default_ssid);
    char default_pass[65]; size_t pass_len = sizeof(default_pass);
    uint8_t ap_enabled;
    esp_err_t wifi_err = load_wifi_config(default_ssid, &ssid_len, default_pass, &pass_len, &ap_enabled);
    /* La clave del AP la genera y guarda wifi_ap_init (aleatoria por pantalla).
     * Aqui solo se MUESTRA lo que haya: sembrarla desde la UI es justo lo que
     * dejaba "12345678" de fabrica en todas las pantallas. El SSID por defecto
     * tambien lo persiste wifi_ap_init. 2026-07-26. */
    if (wifi_err != ESP_OK) {
        strncpy(default_ssid, "VictronConfig", sizeof(default_ssid));
        default_ssid[sizeof(default_ssid) - 1] = '\0';
        default_pass[0] = '\0';
        ap_enabled = 1;
    }


    load_screensaver_settings(&ui->screensaver.enabled,
                              &ui->screensaver.brightness,
                              &ui->screensaver.timeout);
    /* Minimo 1 min: migra un 0 antiguo (para desactivar esta el switch ON/OFF). */
    if (ui->screensaver.timeout < 60) ui->screensaver.timeout = 60;
    load_screensaver_mode(&ui->screensaver.mode, &ui->screensaver.rotate_period_min);
    ui->screensaver.rotate_index = 0;
    ui->screensaver.rotate_timer = NULL;

#if LV_USE_THEME_DEFAULT
    lv_theme_default_init(NULL,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        LV_THEME_DEFAULT_DARK,
        &lv_font_montserrat_28_es
    );
#endif

    ui->tabview   = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 60);
    lv_obj_add_flag(ui->tabview, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(ui->tabview, LV_OBJ_FLAG_SCROLLABLE);

    /* Tarea de beeps (lanzada antes de instalar el handler) para que el
     * click no bloquee el thread LVGL durante los ~300 ms del jingle. */
    if (!s_beep_queue) {
        s_beep_queue = xQueueCreate(1, sizeof(uint8_t));
        if (s_beep_queue) {
            xTaskCreate(ui_beep_task, "ui_beep", 3072, NULL, 4, NULL);
        }
    }

    /* Beep global al pulsar cualquier widget clicable. LV_EVENT_CLICKED
     * burbujea desde el hijo hasta el screen, asi un solo handler en la
     * pantalla activa cubre toda la UI sin tener que tocar cada boton. */
    lv_obj_add_event_cb(lv_scr_act(), ui_global_click_beep_cb,
                        LV_EVENT_CLICKED, NULL);
    /* Estilo de los tabs: fondo oscuro, fuente grande, indicador azul */
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(ui->tabview);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_28_es, 0);
    /* Fondo de la barra de tabs */
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, 0);
    /* Color de texto en estado normal: gris claro */
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(0xBBBBBB), 0);
    /* Color de texto en estado activo: blanco */
    lv_obj_set_style_text_color(tab_btns, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    /* Indicador (linea bajo el activo) en azul */
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_height(tab_btns, 4, LV_PART_INDICATOR);
    lv_obj_set_style_radius(tab_btns, 2, LV_PART_INDICATOR);
    /* Quitar borde inferior por defecto del tabview */
    lv_obj_set_style_border_width(tab_btns, 0, 0);
    ui->tab_live  = lv_tabview_add_tab(ui->tabview, LV_SYMBOL_HOME "  Live");
    ui->tab_settings = lv_tabview_add_tab(ui->tabview, LV_SYMBOL_SETTINGS "  Settings");

    ui->tab_settings_index = lv_obj_get_index(ui->tab_settings);

    /* Ocultar la barra de pestañas: usamos icono en bottom_bar para navegar.
     * Importante: además de HIDDEN hay que poner altura 0 para que el contenido
     * (tab_live/tab_settings) ocupe toda la pantalla. */
    lv_obj_add_flag(tab_btns, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(tab_btns, 0);
    /* Reserva inferior = altura exacta de la bottom_bar (50 px), sin margen
     * muerto, para que el overview aproveche toda la pantalla. El pequeno
     * colchon sobre la barra lo da el pad_bottom del root del overview. */
    lv_obj_set_style_pad_bottom(ui->tab_live, 50, 0);
    lv_obj_set_style_pad_bottom(ui->tab_settings, 62, 0);

    /* Reloj en barra superior — esquina derecha */
    /* Barra inferior unificada: contenedor flex con 4 zonas (reloj | BLE | volumen | temp ext) */
    ui->bottom_bar = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(ui->bottom_bar);
    lv_obj_set_size(ui->bottom_bar, lv_pct(100), 50);
    lv_obj_align(ui->bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ui->bottom_bar, lv_color_hex(0x06080C), 0);
    lv_obj_set_style_bg_opa(ui->bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_layout(ui->bottom_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui->bottom_bar, LV_FLEX_FLOW_ROW);
    /* SPACE_BETWEEN ancla primer y último elemento a los bordes y reparte el
     * resto uniformemente. Combinado con anchos fijos, las posiciones de los
     * iconos son las mismas independientemente del contenido (textos del
     * reloj que cambian de longitud, pills, etc.). */
    lv_obj_set_flex_align(ui->bottom_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(ui->bottom_bar, 12, 0);
    lv_obj_set_style_pad_ver(ui->bottom_bar, 4, 0);
    lv_obj_clear_flag(ui->bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
    /* Que la barra no intercepte clicks (los recoge lo de abajo) */
    lv_obj_clear_flag(ui->bottom_bar, LV_OBJ_FLAG_CLICKABLE);

    /* Reloj — ancho fijo para que el cambio de "00:00" a "00:00 dd/mm/yyyy"
     * no desplace al resto de iconos. */
    ui->lbl_clock = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_clock, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(ui->lbl_clock, lv_color_white(), 0);
    lv_label_set_text(ui->lbl_clock, "00:00");
    lv_obj_set_style_bg_opa(ui->lbl_clock, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_clock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_clock, 4, 0);
    lv_obj_set_style_radius(ui->lbl_clock, 4, 0);
    lv_obj_set_width(ui->lbl_clock, 280);
    lv_obj_set_style_text_align(ui->lbl_clock, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(ui->lbl_clock, LV_LABEL_LONG_CLIP);
    /* Indicador BLE — ancho fijo */
    ui->lbl_ble = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_ble, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->lbl_ble, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_bg_opa(ui->lbl_ble, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_ble, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_ble, 4, 0);
    lv_obj_set_style_radius(ui->lbl_ble, 4, 0);
    lv_obj_set_size(ui->lbl_ble, 44, 38);
    lv_obj_set_style_text_align(ui->lbl_ble, LV_TEXT_ALIGN_CENTER, 0);

    /* Icono de volumen / mute — ancho fijo */
    ui->lbl_volume = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_volume, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ui->lbl_volume, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ui->lbl_volume, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_volume, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_volume, 4, 0);
    lv_obj_set_style_radius(ui->lbl_volume, 4, 0);
    lv_label_set_text(ui->lbl_volume, audio_is_muted() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_size(ui->lbl_volume, 44, 38);
    lv_obj_set_style_text_align(ui->lbl_volume, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui->lbl_volume, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->lbl_volume, volume_btn_event_cb, LV_EVENT_CLICKED, ui);

    /* Icono Wi-Fi (AP) — ancho fijo */
    ui->lbl_wifi = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_wifi, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_bg_opa(ui->lbl_wifi, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_wifi, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_wifi, 4, 0);
    lv_obj_set_style_radius(ui->lbl_wifi, 4, 0);
    lv_label_set_text(ui->lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_size(ui->lbl_wifi, 44, 38);
    lv_obj_set_style_text_align(ui->lbl_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui->lbl_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->lbl_wifi, wifi_btn_event_cb, LV_EVENT_CLICKED, ui);
    /* Icono GPS — mismo tamano y forma que el de Wi-Fi, justo al lado.
     *
     * NO es pulsable, a diferencia del de Wi-Fi: es un indicador y nada mas. En
     * un tactil dentro de un vehiculo en marcha, cualquier cosa que reaccione al
     * roce acaba activandose sin querer (ya paso con el selector de orientacion
     * del nivel en la 3.5"). */
    ui->lbl_gps = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_gps, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_bg_opa(ui->lbl_gps, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_gps, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_gps, 4, 0);
    lv_obj_set_style_radius(ui->lbl_gps, 4, 0);
    lv_label_set_text(ui->lbl_gps, LV_SYMBOL_GPS);
    lv_obj_set_size(ui->lbl_gps, 44, 38);
    lv_obj_set_style_text_align(ui->lbl_gps, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ui->lbl_gps, lv_color_hex(0x666666), 0);

    /* Color inicial segun NVS */
    {
        nvs_handle_t h;
        uint8_t en = 1;
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "enabled", &en);
            nvs_close(h);
        }
        lv_obj_set_style_text_color(ui->lbl_wifi,
            en ? lv_color_hex(0x4FC3F7) : lv_color_hex(0x666666), 0);
    }
    /* Indicador "frigo con excedente solar activo". Oculto hasta que el
     * modo excedente solar del frigo esta realmente tirando de el. */
    ui->lbl_solar = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_solar, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->lbl_solar, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_bg_opa(ui->lbl_solar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_solar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_solar, 4, 0);
    lv_obj_set_style_radius(ui->lbl_solar, 4, 0);
    lv_label_set_text(ui->lbl_solar, LV_SYMBOL_CHARGE " 12V sol");
    lv_obj_set_size(ui->lbl_solar, 110, 38);
    lv_obj_set_style_text_align(ui->lbl_solar, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);

    /* Botón de navegación Live↔Settings — estilo discreto idéntico a los
     * demás iconos de estado de la barra (sin fondo destacado). */
    ui->btn_nav = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->btn_nav, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ui->btn_nav, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(ui->btn_nav, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_bg_opa(ui->btn_nav, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->btn_nav, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->btn_nav, 4, 0);
    lv_obj_set_style_radius(ui->btn_nav, 4, 0);
    lv_obj_set_size(ui->btn_nav, 44, 38);
    lv_obj_set_style_text_align(ui->btn_nav, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui->btn_nav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->btn_nav, nav_btn_event_cb, LV_EVENT_CLICKED, ui);
    /* Capturar CUALQUIER cambio del tab activo (botón, swipe, programático)
     * para mantener el icono de la barra inferior siempre coherente con
     * la pestaña visible. Sin esto el icono se queda desincronizado al
     * hacer swipe horizontal entre Live y Settings. */
    lv_obj_add_event_cb(ui->tabview, nav_icon_sync_cb, LV_EVENT_VALUE_CHANGED, ui);

    lv_timer_create(volume_icon_timer_cb, 500, ui);
    lv_timer_create(solar_indicator_timer_cb, 2000, ui);

    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_live, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tab_settings, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    /* Auto-vuelta a Live tras 60 s sin tocar la pantalla cuando se está en
     * Settings. Comprueba cada 5 s. Al volver, reseteamos el menú de
     * Settings al main page para que la próxima entrada arranque ahí. */
    lv_timer_create(settings_auto_return_cb, 5000, ui);

    ui->keyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(ui->keyboard, LV_HOR_RES, LV_VER_RES/2);
    lv_obj_align(ui->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);

    // Styles
    lv_style_init(&ui->styles.small);
    /* Use montserrat 22 for titles as requested */
lv_style_set_text_font(&ui->styles.small, &lv_font_montserrat_28_es);
    lv_style_set_text_color(&ui->styles.small, lv_color_white());

    lv_style_init(&ui->styles.medium);
    lv_style_set_text_font(&ui->styles.medium, &lv_font_montserrat_36);
    lv_style_set_text_color(&ui->styles.medium, lv_color_white());

    lv_style_init(&ui->styles.big);
    lv_style_set_text_font(&ui->styles.big, &lv_font_montserrat_46);
    lv_style_set_text_color(&ui->styles.big, lv_color_white());

    lv_style_init(&ui->styles.value);
lv_style_set_text_font(&ui->styles.value, &lv_font_montserrat_32);
    lv_style_set_text_color(&ui->styles.value, lv_color_white());

    // Create default battery view instead of "No live data" label
    ui->default_view = ui_default_battery_view_create(ui, ui->tab_live);
    // Crear también la vista Overview (paralela a default_view)
    ui->overview_view = ui_overview_view_create(ui, ui->tab_live);

    /* Mostrar la vista inicial según la selección guardada */
    if (ui->view_selection.mode == UI_VIEW_MODE_OVERVIEW) {
        if (ui->overview_view && ui->overview_view->show)
            ui->overview_view->show(ui->overview_view);
        if (ui->default_view && ui->default_view->hide)
            ui->default_view->hide(ui->default_view);
    } else {
        if (ui->default_view && ui->default_view->show)
            ui->default_view->show(ui->default_view);
        if (ui->overview_view && ui->overview_view->hide)
            ui->overview_view->hide(ui->overview_view);
    }
    
    // Keep the old label for compatibility but hide it
    ui->lbl_no_data = lv_label_create(ui->tab_live);
    lv_label_set_text(ui->lbl_no_data, "No live data received yet");
    lv_obj_add_style(ui->lbl_no_data, &ui->styles.medium, 0);
    lv_label_set_long_mode(ui->lbl_no_data, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->lbl_no_data, lv_pct(90));
    lv_obj_set_style_text_align(ui->lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui->lbl_no_data);
    lv_obj_add_flag(ui->lbl_no_data, LV_OBJ_FLAG_HIDDEN); // Hide by default

    ui_settings_panel_init(ui, default_ssid, default_pass, ap_enabled);
    ui_settings_screensaver_create_timer(ui);

    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(lv_scr_act(), tabview_touch_event_cb, LV_EVENT_GESTURE, ui);

    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->tabview, tabview_touch_event_cb, LV_EVENT_GESTURE, ui);
    lv_timer_create(clock_timer_cb, 30000, ui);
    lv_timer_create(ble_indicator_timer_cb, 1000, ui);
    lv_timer_create(gps_indicator_timer_cb, 1000, ui);
    s_idle_to_live_timer = lv_timer_create(idle_to_live_timer_cb,
                                           IDLE_TO_LIVE_TIMEOUT_MS, ui);
    clock_timer_cb(NULL);
    /* NO desbloqueamos aqui: el lock LVGL lo toma y libera el llamante
     * (app_main), que ejecuta splash_show() tras ui_init bajo el mismo lock. */
}

void ui_on_panel_data(const victron_data_t *d) {
    if (d == NULL) {
        return;
    }

    /* Snapshot global para el dashboard del portal web. */
    dashboard_state_on_record(d);
    /* Trip computer: integra cargas/descargas del BMV/Lynx. */
    if (d->type == VICTRON_BLE_RECORD_BATTERY_MONITOR) {
        const victron_record_battery_monitor_t *b = &d->record.battery;
        trip_computer_on_battery(b->battery_current_milli, b->battery_voltage_centi);
        solar_daily_on_battery(b->battery_current_milli, b->battery_voltage_centi);
    } else if (d->type == VICTRON_BLE_RECORD_LYNX_SMART_BMS) {
        const victron_record_lynx_smart_bms_t *b = &d->record.lynx;
        trip_computer_on_battery((int32_t)b->battery_current_deci * 100,
                                 b->battery_voltage_centi);
    }
    /* Aporte de la placa solar (MPPT): se acumula aparte, medido en la salida
     * del cargador hacia la bateria. */
    if (d->type == VICTRON_BLE_RECORD_SOLAR_CHARGER) {
        const victron_record_solar_charger_t *sc = &d->record.solar;
        trip_computer_on_solar((int32_t)sc->battery_current_deci * 100,
                               sc->battery_voltage_centi);
    }

    ui_state_t *ui = &g_ui;

    /* BLE rx corre en la task NimBLE; con lvgl_port_lock(0) (= portMAX_DELAY)
     * un cuelgue de LVGL bloquearia para siempre el rx BLE. Acotamos a 100 ms;
     * si no se obtiene, abandonamos este frame y dejamos que el siguiente
     * lo intente. */
    if (!lvgl_port_lock(100)) {
        return;
    }

    if (ui->lbl_ble) {
        lv_label_set_text(ui->lbl_ble, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x00C851), 0);
    }
    s_last_ble_data_us = esp_timer_get_time();

    /* Cache del ultimo record por fuente para el detalle instantaneo de las
     * cards del Overview (Solar/Bateria/DC-DC). */
    switch (d->type) {
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:   s_last_solar = *d;   s_has_solar = true;   break;
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: s_last_battery = *d; s_has_battery = true; break;
        case VICTRON_BLE_RECORD_DCDC_CONVERTER:
        case VICTRON_BLE_RECORD_ORION_XS:        s_last_dcdc = *d;    s_has_dcdc = true;    break;
        default: break;
    }

    /* Battery history + deteccion de cruce de alarma SoC: ver ui/ble_ingest.c. */
    ble_ingest_feed_history(d);

    if (!ui->has_received_data) {
        ui->has_received_data = true;
        if (ui->lbl_no_data) {
            lv_obj_add_flag(ui->lbl_no_data, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Always update the default view with incoming data (it handles multiple device types)
    if (ui->default_view && ui->default_view->update) {
        ui->default_view->update(ui->default_view, d);
    }
    // Overview también se alimenta siempre — mantiene state interno consolidado
    if (ui->overview_view && ui->overview_view->update) {
        ui->overview_view->update(ui->overview_view, d);
    }

    const char *type_str = device_type_name(d->type);
    
    // Update legacy labels (if they exist for compatibility)
    if (ui->lbl_device_type) {
        lv_label_set_text_fmt(ui->lbl_device_type, "Device: %s", type_str);
    }

    ui->current_product_id = d->product_id;
    if (ui->lbl_product_name) {
        if (d->product_id != 0) {
            const char *prod_name = victron_product_name(d->product_id);
            if (prod_name != NULL) {
                lv_label_set_text_fmt(ui->lbl_product_name,
                                      "Product: %s (0x%04X)",
                                      prod_name,
                                      (unsigned)d->product_id);
            } else {
                lv_label_set_text_fmt(ui->lbl_product_name,
                                      "Product: 0x%04X",
                                      (unsigned)d->product_id);
            }
        } else {
            lv_label_set_text(ui->lbl_product_name, "Product: --");
        }
    }
    
    // Prepare product info for device status updates
    char product_info[128] = {0};
    if (d->product_id != 0) {
        const char *prod_name = victron_product_name(d->product_id);
        if (prod_name != NULL) {
            snprintf(product_info, sizeof(product_info), "%s (0x%04X)", prod_name, (unsigned)d->product_id);
        } else {
            snprintf(product_info, sizeof(product_info), "0x%04X", (unsigned)d->product_id);
        }
    } else {
        strcpy(product_info, "--");
    }

    ensure_device_layout(ui, d->type);

    if (ui->active_view && ui->active_view->update) {
        ui->active_view->update(ui->active_view, d);
        
        // Prepare detailed status information based on device type
        char detailed_status[256] = {0};
        ui_prepare_detailed_device_status(d, detailed_status, sizeof(detailed_status));
        
        // Update device activity tracking
        ui_update_device_activity(ui, ui->current_device_mac);
        
        // Update successful data reception status in Victron Keys page
        ui_settings_panel_update_victron_device_status(ui, ui->current_device_mac, type_str, product_info, detailed_status);
    } else {
        // Update error status in Victron Keys page
        const char *error_msg = "No renderer for device type";
        if (d->type == VICTRON_BLE_RECORD_TEST) {
            error_msg = "Unknown device type";
        }
        
        // Update legacy error label (if it exists)
        if (ui->lbl_error) {
            lv_label_set_text(ui->lbl_error, error_msg);
        }
        
        // Update error status for this device in Victron Keys page
        ui_settings_panel_update_victron_device_status(ui, ui->current_device_mac, type_str, product_info, error_msg);
    }

    lvgl_port_unlock();
}

void ui_force_view_update(void)
{
    ui_state_t *ui = &g_ui;
    /* Llamado desde el dropdown de "vista por defecto"; timeout amplio. */
    if (!lvgl_port_lock(500)) {
        return;
    }

    victron_record_type_t saved_type = ui->current_device_type;
    ui->current_device_type = VICTRON_BLE_RECORD_TEST;
    ensure_device_layout(ui, saved_type);

    lvgl_port_unlock();
}

static void clock_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : ui_get_state();
    if (!ui || !ui->lbl_clock) return;

    /* Usar el reloj del sistema en lugar de leer el RTC cada vez */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);

    if (t.tm_year > 100) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%02d:%02d  %02d/%02d/%04d",
                 t.tm_hour, t.tm_min,
                 t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        lv_label_set_text(ui->lbl_clock, buf);
    }
}

void ui_refresh_clock(void)
{
    if (lvgl_port_lock(50)) {
        clock_timer_cb(NULL);
        lvgl_port_unlock();
    }
}

/* ── Overlay de "actualizando firmware" durante la OTA ──────────────────────
 * Fondo opaco a pantalla completa en lv_layer_top() (por encima de cualquier
 * tab/modal) con un texto grande centrado. Sin animacion ni redibujado
 * periodico: al no cambiar nada en pantalla mientras dura, no vuelve a
 * generar el tearing que se ve en el resto de la UI bajo la misma carga de
 * flash (ver watchdog.c / project_ota_parpadeo_azul_confirmado). Bloquea el
 * touch (LV_OBJ_FLAG_CLICKABLE) para que nadie pueda tocar nada mientras la
 * OTA esta en marcha. */
static lv_obj_t *s_ota_overlay = NULL;
static lv_obj_t *s_ota_overlay_lbl = NULL;

void ui_ota_overlay_show(const char *msg)
{
    if (!lvgl_port_lock(300)) return;
    if (!s_ota_overlay) {
        s_ota_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_ota_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_ota_overlay, lv_color_hex(0x06080C), 0);
        lv_obj_set_style_bg_opa(s_ota_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ota_overlay, 0, 0);
        lv_obj_set_style_radius(s_ota_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_ota_overlay, 0, 0);
        lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_CLICKABLE);

        s_ota_overlay_lbl = lv_label_create(s_ota_overlay);
        lv_obj_set_style_text_font(s_ota_overlay_lbl, &lv_font_montserrat_28_es, 0);
        lv_obj_set_style_text_color(s_ota_overlay_lbl, lv_color_white(), 0);
        lv_label_set_long_mode(s_ota_overlay_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_ota_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_ota_overlay_lbl, lv_pct(80));
        lv_obj_center(s_ota_overlay_lbl);
    }
    lv_label_set_text(s_ota_overlay_lbl, msg);
    lvgl_port_unlock();
}

void ui_ota_overlay_hide(void)
{
    if (!s_ota_overlay) return;
    if (!lvgl_port_lock(300)) return;
    lv_obj_del(s_ota_overlay);
    s_ota_overlay = NULL;
    s_ota_overlay_lbl = NULL;
    lvgl_port_unlock();
}

/* ── Auto-volver a Live tras 60 s sin actividad del usuario ──
 * Disparado por s_idle_to_live_timer; reset en ui_notify_user_activity
 * (que solo se llama desde LV_EVENT_PRESSED real del usuario).
 * Cierra todos los overlays y resetea el menu Settings a su pagina
 * principal antes de cambiar de tab para que la proxima entrada
 * arranque en main, no en la subpagina donde quedo. */
static void idle_to_live_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = t ? (ui_state_t *)t->user_data : ui_get_state();
    if (!ui || !ui->tabview) return;
    /* No interferir con el screensaver: si está activo, deja que rote/atenúe */
    if (ui->screensaver.active) return;
    /* Si estamos en Settings, cerrar submenu y dropdowns antes de salir */
    if (lv_tabview_get_tab_act(ui->tabview) == ui->tab_settings_index) {
        ui_frigo_panel_close_dropdowns();
        ui_settings_panel_go_to_main();
    }
    if (lv_tabview_get_tab_act(ui->tabview) != 0) {
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
    }
    /* Cerrar overlays si quedaran abiertos (chart frigo, histórico batería) */
    ui_close_chart_screen();
    ui_close_battery_history_screen();
    /* Detalle de card abierto -> volver a la principal tras 1 min sin tocar. */
    ui_close_card_detail();
}

void ui_notify_user_activity(void)
{
    ui_state_t *ui = &g_ui;
    if (s_idle_to_live_timer) lv_timer_reset(s_idle_to_live_timer);
    ui_settings_panel_on_user_activity(ui);
}

/* ── Detalle a pantalla completa de una card del Overview ──────────────
 * Reusa las vistas de detalle ya existentes (registry) como active_view y un
 * unico boton flotante de "volver". El retorno a la principal tras 1 min lo
 * hace el idle_to_live_timer (que llama a ui_close_card_detail). */
static void card_detail_back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_close_card_detail();
}

static void ui_card_detail_ensure_back_btn(void)
{
    if (s_card_detail_back_btn) return;
    lv_obj_t *btn = lv_btn_create(lv_layer_top());
    lv_obj_set_size(btn, 54, 54);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E2635), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x4A5568), 0);
    lv_obj_add_event_cb(btn, card_detail_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    s_card_detail_back_btn = btn;
}

void ui_show_card_detail(ui_state_t *ui, victron_record_type_t category)
{
    if (!ui) return;
    /* Resolver tipo concreto + ultimo dato conocido para pintar al instante. */
    victron_record_type_t type = category;
    const victron_data_t *cached = NULL;
    switch (category) {
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:
            if (s_has_solar) cached = &s_last_solar;
            break;
        case VICTRON_BLE_RECORD_BATTERY_MONITOR:
            if (s_has_battery) cached = &s_last_battery;
            break;
        case VICTRON_BLE_RECORD_DCDC_CONVERTER:
        case VICTRON_BLE_RECORD_ORION_XS:
            if (s_has_dcdc) { cached = &s_last_dcdc; type = s_last_dcdc.type; }
            break;
        default:
            break;
    }
    ui_device_view_t *view = ui_view_registry_ensure(ui, type, ui->tab_live);
    if (!view || !view->show) return;

    if (ui->active_view && ui->active_view->hide) ui->active_view->hide(ui->active_view);
    if (ui->default_view && ui->default_view->hide) ui->default_view->hide(ui->default_view);
    if (ui->overview_view && ui->overview_view->hide) ui->overview_view->hide(ui->overview_view);
    view->show(view);
    ui->active_view = view;
    s_card_detail_active = true;

    if (cached && view->update) view->update(view, cached);

    ui_card_detail_ensure_back_btn();
    lv_obj_clear_flag(s_card_detail_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_card_detail_back_btn);

    /* Arrancar el minuto desde cero al abrir. */
    if (s_idle_to_live_timer) lv_timer_reset(s_idle_to_live_timer);
}

void ui_close_card_detail(void)
{
    if (!s_card_detail_active) return;
    ui_state_t *ui = &g_ui;
    s_card_detail_active = false;
    if (ui->active_view && ui->active_view->hide) ui->active_view->hide(ui->active_view);
    ui->active_view = NULL;
    if (ui->overview_view && ui->overview_view->show) ui->overview_view->show(ui->overview_view);
    if (s_card_detail_back_btn) lv_obj_add_flag(s_card_detail_back_btn, LV_OBJ_FLAG_HIDDEN);
}

bool ui_screensaver_is_active(void)
{
    return g_ui.screensaver.active;
}

void ui_alarm_interrupt_screensaver(void)
{
    ui_state_t *ui = &g_ui;
    /* Solo si el salvapantallas esta rotando: sacamos al usuario de la
     * rotacion y mostramos Live + Overview, donde la alarma parpadea. */
    if (ui->screensaver.mode != UI_SCREENSAVER_MODE_ROTATE) return;
    if (!ui->screensaver.active) return;
    /* Reutiliza el wake: sale del salvapantallas, para la rotacion, restaura
     * brillo y vuelve a la pestaña Live. */
    ui_settings_panel_on_user_activity(ui);
    /* Forzar la vista Overview dentro de Live (la alarma se visualiza ahi).
     * Solo en memoria; no persiste la preferencia del usuario. */
    ui->view_selection.mode = UI_VIEW_MODE_OVERVIEW;
    ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
    ESP_LOGW(TAG_UI, "Alarma activa: rotacion interrumpida -> Live/Overview");
}

/* Sincroniza el icono del botón nav con el tab activo del tabview.
 * Se invoca en LV_EVENT_VALUE_CHANGED del tabview, así cubre los 3 paths
 * de cambio: pulsar btn_nav, swipe horizontal, set_act programático. */
static void nav_icon_sync_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui || !ui->tabview || !ui->btn_nav) return;
    uint16_t cur = lv_tabview_get_tab_act(ui->tabview);
    /* En Live (tab 0) → mostrar SETTINGS (acción "ir a Settings").
     * En Settings → mostrar HOME (acción "volver a Live"). */
    lv_label_set_text(ui->btn_nav,
        (cur == ui->tab_settings_index) ? LV_SYMBOL_HOME : LV_SYMBOL_SETTINGS);
}

/* Timer que vuelve a Live tras 60 s sin tocar la pantalla cuando se está
 * en Settings. Si NO estamos en Settings, no hace nada. */
static void settings_auto_return_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->tabview) return;
    if (lv_tabview_get_tab_act(ui->tabview) != ui->tab_settings_index) return;
    if (lv_disp_get_inactive_time(NULL) < 60000) return;

    /* Cerrar dropdowns abiertos del panel Frigo: la lista flotante de un
     * dropdown LVGL no se cierra sola y, sin esto, se queda visible sobre
     * la vista Live tras el auto-return. */
    ui_frigo_panel_close_dropdowns();

    /* Reset menú a página principal de Settings antes de salir, para que
     * la próxima entrada arranque ahí (no en la subpágina donde quedó).
     * El icono lo actualiza nav_icon_sync_cb tras set_act. */
    ui_settings_panel_go_to_main();
    lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
}

/* Toggle Live ↔ Settings al pulsar el icono de la barra inferior */
static void nav_btn_event_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui || !ui->tabview) return;
    uint16_t cur = lv_tabview_get_tab_act(ui->tabview);
    uint16_t next = (cur == 0) ? ui->tab_settings_index : 0;
    /* Si estamos saliendo de Settings hacia Live, reseteamos el menu
     * a la pagina principal para que la proxima entrada arranque ahi y
     * no en la subpagina (Display, Wi-Fi, etc.) donde se quedo. */
    if (cur == ui->tab_settings_index && next == 0) {
        ui_frigo_panel_close_dropdowns();
        ui_settings_panel_go_to_main();
    }
    lv_tabview_set_act(ui->tabview, next, LV_ANIM_OFF);
    /* El icono lo sincroniza nav_icon_sync_cb tras LV_EVENT_VALUE_CHANGED. */
}

/* Toggle mute/unmute al pulsar el icono de volumen — mismo efecto exacto
 * que el switch "Silenciar avisos" en Settings/Sonido. El refresco visual
 * del label lo hace volume_icon_timer_cb (cada 500 ms). */
static void volume_btn_event_cb(lv_event_t *e)
{
    (void)lv_event_get_user_data(e);
    /* Misma logica que el switch "Silenciar avisos": guarda/restaura el volumen
     * y sincroniza slider, etiqueta y switch de Settings (si estan creados). */
    ui_settings_apply_mute(!audio_is_muted());
}

/* Toggle Wi-Fi AP on/off al pulsar el icono — comportamiento idéntico al
 * switch del panel Settings: guarda NVS, sincroniza el switch y aplica el
 * cambio EN CALIENTE (sin reiniciar la placa). */
static void wifi_btn_event_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui) return;

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t en = 1;
    nvs_get_u8(h, "enabled", &en);
    en = en ? 0 : 1;
    nvs_set_u8(h, "enabled", en);
    esp_err_t err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG_UI, "wifi enabled no persistio: %s", esp_err_to_name(err));
    nvs_close(h);

    ui_wifi_set_enabled_cache(en);   /* el icono de la barra cachea el flag */

    /* Sincronizar el switch del Settings con el nuevo estado. */
    if (ui->wifi.ap_enable) {
        if (en) lv_obj_add_state(ui->wifi.ap_enable, LV_STATE_CHECKED);
        else    lv_obj_clear_state(ui->wifi.ap_enable, LV_STATE_CHECKED);
    }

    config_server_request_wifi_apply();
}

void ui_set_ble_mac(const uint8_t *mac) {
    // Format MAC as "XX:XX:XX:XX:XX:XX"
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
    ui_state_t *ui = &g_ui;
    /* Llamado desde NimBLE rx; acotamos para no bloquear si LVGL esta colgado. */
    if (!lvgl_port_lock(100)) {
        return;
    }

    strcpy(ui->current_device_mac, mac_str);
    ui_settings_panel_set_mac(ui, mac_str);
    lvgl_port_unlock();
}

void ensure_device_layout(ui_state_t *ui, victron_record_type_t type)
{
    if (ui == NULL) {
        return;
    }

    /* Mientras hay un detalle de card abierto, congelamos la seleccion de
     * vista: el detalle es la active_view y se sigue alimentando por
     * active_view->update; no dejamos que el siguiente advert lo cambie. */
    if (s_card_detail_active) {
        return;
    }

    // Check if we should use manual view selection instead of auto detection
    if (ui->view_selection.mode != UI_VIEW_MODE_AUTO) {
        // Manual view mode selected - determine which view to show
        victron_record_type_t target_type = VICTRON_BLE_RECORD_TEST;
        bool show_default = true;
        bool show_overview = false;

        switch (ui->view_selection.mode) {
            case UI_VIEW_MODE_OVERVIEW:
                show_overview = true;
                show_default = false;
                break;
            case UI_VIEW_MODE_DEFAULT_BATTERY:
                show_default = true;
                break;
            case UI_VIEW_MODE_SOLAR_CHARGER:
                target_type = VICTRON_BLE_RECORD_SOLAR_CHARGER;
                show_default = false;
                break;
            case UI_VIEW_MODE_BATTERY_MONITOR:
                target_type = VICTRON_BLE_RECORD_BATTERY_MONITOR;
                show_default = false;
                break;
            case UI_VIEW_MODE_INVERTER:
                target_type = VICTRON_BLE_RECORD_INVERTER;
                show_default = false;
                break;
            case UI_VIEW_MODE_DCDC_CONVERTER:
                target_type = VICTRON_BLE_RECORD_DCDC_CONVERTER;
                show_default = false;
                break;
            default:
                show_default = true;
                break;
        }

        if (show_overview) {
            /* Mostrar Overview, ocultar default y cualquier active_view */
            if (ui->active_view && ui->active_view->hide) {
                ui->active_view->hide(ui->active_view);
            }
            ui->active_view = NULL;
            if (ui->default_view && ui->default_view->hide) {
                ui->default_view->hide(ui->default_view);
            }
            if (ui->overview_view && ui->overview_view->show) {
                ui->overview_view->show(ui->overview_view);
            }
        } else if (show_default) {
            // Show default battery view
            if (ui->active_view && ui->active_view->hide) {
                ui->active_view->hide(ui->active_view);
            }
            ui->active_view = NULL;
            if (ui->overview_view && ui->overview_view->hide) {
                ui->overview_view->hide(ui->overview_view);
            }
            if (ui->default_view && ui->default_view->show) {
                ui->default_view->show(ui->default_view);
            }
        } else {
            // Show specific view type regardless of received data type
            if (ui->active_view && ui->active_view->hide) {
                ui->active_view->hide(ui->active_view);
            }

            ui->active_view = NULL;
            if (ui->overview_view && ui->overview_view->hide) {
                ui->overview_view->hide(ui->overview_view);
            }
            ui_device_view_t *view = ui_view_registry_ensure(ui, target_type, ui->tab_live);
            if (view && view->show) {
                if (ui->default_view && ui->default_view->hide) {
                    ui->default_view->hide(ui->default_view);
                }
                view->show(view);
                ui->active_view = view;
            } else {
                // Fallback to default view if specific view not available
                if (ui->default_view && ui->default_view->show) {
                    ui->default_view->show(ui->default_view);
                }
                ESP_LOGW(TAG_UI, "Requested view type 0x%02X not available, showing default", (unsigned)target_type);
            }
        }

        ui->current_device_type = (show_overview || show_default)
            ? VICTRON_BLE_RECORD_TEST : target_type;
        return;
    }

    // Auto detection mode - original logic
    if (type == ui->current_device_type) {
        return;
    }

    if (ui->active_view && ui->active_view->hide) {
        ui->active_view->hide(ui->active_view);
    }

    ui->active_view = NULL;

    ui_device_view_t *view = ui_view_registry_ensure(ui, type, ui->tab_live);
    if (view && view->show) {
        // Hide default view when showing specific device view
        if (ui->default_view && ui->default_view->hide) {
            ui->default_view->hide(ui->default_view);
        }
        view->show(view);
        ui->active_view = view;
    } else {
        // No specific view available, show default view
        if (ui->default_view && ui->default_view->show) {
            ui->default_view->show(ui->default_view);
        }
        if (type != VICTRON_BLE_RECORD_TEST) {
            ESP_LOGW(TAG_UI, "No view available for device type 0x%02X, showing default", (unsigned)type);
        }
    }

    ui->current_device_type = type;
}

static const char *device_type_name(victron_record_type_t type)
{
    return ui_view_registry_name(type);
}

static void tabview_touch_event_cb(lv_event_t *e) {
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED || ui->keyboard == NULL) {
        return;
    }

    if (lv_obj_has_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    if (obj_is_descendant(target, ui->keyboard)) {
        return;
    }

    if (ui->wifi.password_toggle != NULL &&
        obj_is_descendant(target, ui->wifi.password_toggle)) {
        return;
    }

    lv_obj_t *ta = lv_keyboard_get_textarea(ui->keyboard);
    if (obj_is_descendant(target, ta)) {
        return;
    }

    if (ta != NULL) {
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_event_send(ta, LV_EVENT_DEFOCUSED, NULL);
    } else {
        lv_keyboard_set_textarea(ui->keyboard, NULL);
        lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_obj_set_height(ui->tabview, screen_h);
        lv_obj_update_layout(ui->tabview);
    }
}

// force font update
// force tab font

ui_state_t *ui_get_state(void) { return &g_ui; }

/* Estado de la alarma del congelador, calculado con el criterio robusto
 * (subiendo >=N min + T>umbral) en main.c::frigo_update_cb. Es la unica
 * fuente de verdad: la vista Overview lo lee via ui_get_freezer_alarm()
 * en lugar de re-evaluar el umbral por su cuenta. */
static bool s_freezer_alarm_active = false;

void ui_set_freezer_alarm(ui_state_t *ui, bool active)
{
    /* La alarma del congelador ya se senaliza dentro de la vista Overview:
     * la temperatura T_Congelador parpadea en rojo y dispara el patron
     * sonoro (con mute al pulsar). No usamos borde a pantalla completa
     * para no tapar el resto de la UI. */
    (void)ui;
    s_freezer_alarm_active = active;
    if (active) ESP_LOGW("UI", "ALARMA CONGELADOR activa");
}

bool ui_get_freezer_alarm(void)
{
    return s_freezer_alarm_active;
}

static void ble_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_ble) return;
    int64_t now = esp_timer_get_time();
    int64_t age_ms = (now - s_last_ble_data_us) / 1000;
    /* Sin datos nunca recibidos o > 5s sin actualizacion -> gris */
    if (s_last_ble_data_us == 0 || age_ms > 5000) {
        lv_label_set_text(ui->lbl_ble, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(ui->lbl_ble, lv_color_hex(0x888888), 0);
    }
}

/* Icono del GPS: TRES estados, no dos.
 *
 *   gris     no llega nada del modulo -> mirar el cable
 *   naranja  habla, pero todavia sin posicion -> esta buscando, hay que esperar
 *   verde    posicion valida
 *
 * Separar "no esta" de "buscando" es lo util: recien encendido y bajo techo, un
 * GPS puede tardar minutos en fijar. Con solo dos estados, esos minutos y un
 * cable suelto se verian igual, y se acabaria desmontando el salpicadero para
 * nada. */
static void gps_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_gps) return;

    gps_data_t g;
    gps_get(&g);

    uint32_t color;
    if      (!g.hay_datos) color = 0x666666;   /* gris */
    else if (!g.hay_fix)   color = 0xFF9800;   /* naranja */
    else                   color = 0x4CD964;   /* verde */
    lv_obj_set_style_text_color(ui->lbl_gps, lv_color_hex(color), 0);
}

/* Pone en la casilla de Ajustes el SSID que de verdad usa el AP, que es el
 * guardado en NVS (lo mismo que lee wifi_ap_init).
 *
 * ANTES SE LO INVENTABA a partir de la MAC ("ESP_%02X%02X%02X" -> "ESP_DC078D")
 * y lo escribia encima de lo que hubiera. Eso RENOMBRABA EL AP: la casilla se
 * guarda sola al perder el foco (settings_wifi.c, DEFOCUSED/READY), asi que
 * bastaba con abrir esa pantalla y tocarla para que el AP dejara de llamarse
 * como se llamaba. Paso el 21-ago-2026: un AP que llevaba semanas como
 * "VictronConfig" amanecio como "ESP_DC078D" y el satelite dejo de encontrarlo
 * (udp_rx: Desconectado reason=201).
 *
 * Si no hay nada guardado se deja el mismo valor de fabrica que usa el AP, no
 * el nombre del chip: lo que se muestre aqui tiene que ser lo que el AP va a
 * anunciar, porque es lo que se acaba guardando. */
void ui_update_wifi_ssid(ui_state_t *ui)
{
    if (!ui || !ui->wifi.ssid) return;

    char ssid[33] = {0};
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &len) != ESP_OK) ssid[0] = '\0';
        nvs_close(h);
    }
    if (ssid[0] == '\0') strcpy(ssid, "VictronConfig");

    if (lvgl_port_lock(50)) {
        lv_textarea_set_text(ui->wifi.ssid, ssid);
        lvgl_port_unlock();
    }
}

