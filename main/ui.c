/* ui.c */
#include "ui.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "config_server.h"
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
#include "ui/frigo_panel.h"
#include "ui/gallery.h"
#include "ne185/ne185.h"
#include "nvs_flash.h"
#include "config_storage.h"
#include <stdio.h>
#include "ui/ui_state.h"
#include "ui/device_view.h"
#include "ui/view_registry.h"
#include "ui/settings_panel.h"
#include "ui/view_default_battery.h"
#include "ui/view_overview.h"
#include "rtc_rx8025t.h"
#include "datalogger.h"
#include "dashboard_state.h"
#include "trip_computer.h"
#include "solar_daily.h"
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
static void ui_enqueue_jingle(audio_jingle_t j, bool priority)
{
    if (!s_beep_queue) return;
    uint8_t v = (uint8_t)j;
    if (priority) xQueueOverwrite(s_beep_queue, &v);
    else          xQueueSend(s_beep_queue, &v, 0);
}

/* Beep corto al pulsar cualquier widget clicable. Encola y vuelve
 * inmediatamente (no bloquea el thread LVGL). */
static void ui_global_click_beep_cb(lv_event_t *e)
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
static void ensure_device_layout(ui_state_t *ui, victron_record_type_t type);
static const char *device_type_name(victron_record_type_t type);
static void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size);
static void ui_update_device_activity(ui_state_t *ui, const char *mac_address);
static void ui_check_device_timeouts(lv_timer_t *timer);
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

/* Altura fija (mA) del pulso on/off del Orion Tr en el log de bateria: el Orion
 * Tr (0x04) NO reporta corriente, solo estado, asi que dibujamos esta altura
 * mientras carga y 0 cuando no. Sirve para ver CUANDO y CUANTO tiempo carga; NO
 * es corriente real (ajustable). */
#define ORION_TR_ON_MILLIAMPS  10000   /* 10 A */
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
    
    // Create timer to check for device timeouts (check every 10 seconds)
    ui->device_timeout_timer = lv_timer_create(ui_check_device_timeouts, 10000, ui);

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

    /* Battery history: alimenta el modulo con la corriente del dispositivo */
    switch (d->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            battery_history_update_latest(BH_SRC_BATTERY_MONITOR,
                d->record.battery.battery_current_milli,
                d->record.battery.battery_voltage_centi);
            /* Deteccion cruce de SoC */
            uint16_t soc_dp = d->record.battery.soc_deci_percent;
            if (soc_dp != 0xFFFF) {
                int soc_pct = soc_dp / 10;
                int crit_th = alerts_get_soc_critical();
                int warn_th = alerts_get_soc_warning();
                static int s_last_soc = -1;
                static bool s_crit_active = false;
                static bool s_warn_active = false;
                if (s_last_soc >= 0) {
                    /* Cruce a la baja del umbral critico */
                    if (s_last_soc >= crit_th && soc_pct < crit_th && !s_crit_active) {
                        s_crit_active = true;
                        /* Encolar (no sonar aqui): esto corre en la task NimBLE
                         * con el lock LVGL cogido; sonar sincrono lo congelaria
                         * ~0.3-1s. La task de audio lo toca fuera del lock. */
                        ui_enqueue_jingle(AUDIO_JINGLE_CRITICAL, true);
                    }
                    /* Recuperacion */
                    if (soc_pct >= crit_th + 2) s_crit_active = false;
                    /* Cruce a la baja del warning (solo si no ya en critico) */
                    if (s_last_soc >= warn_th && soc_pct < warn_th && soc_pct >= crit_th && !s_warn_active) {
                        s_warn_active = true;
                        ui_enqueue_jingle(AUDIO_JINGLE_WARNING, true);
                    }
                    if (soc_pct >= warn_th + 2) s_warn_active = false;
                }
                s_last_soc = soc_pct;
            }
            break;
        }
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:
            /* Con la tension se puede dibujar tambien la potencia que entra a
             * la bateria, y compararla con la que da el panel. */
            battery_history_update_latest(BH_SRC_SOLAR_CHARGER,
                (int32_t)d->record.solar.battery_current_deci * 100,
                (int32_t)d->record.solar.battery_voltage_centi);
            /* Potencia del PANEL (produccion real): va aparte de la corriente
             * que entra a la bateria, porque parte puede ir directa al consumo. */
            battery_history_update_pv((int32_t)d->record.solar.pv_power_w);
            solar_daily_on_pv((int32_t)d->record.solar.pv_power_w);
            break;
        case VICTRON_BLE_RECORD_ORION_XS:
            /* Corriente Y tension de salida (lado bateria). Con la tension se
             * puede saber cuantos kWh ha metido el DC-DC, no solo cuando estuvo
             * cargando. */
            battery_history_update_latest(BH_SRC_ORION_XS,
                (int32_t)d->record.orion.output_current_deci * 100,
                (int32_t)d->record.orion.output_voltage_centi);
            break;
        case VICTRON_BLE_RECORD_AC_CHARGER:
            /* Idem para el cargador de 230 V: con su tension se puede repartir
             * cuanto ha cargado cada fuente. */
            battery_history_update_latest(BH_SRC_AC_CHARGER,
                (int32_t)d->record.ac_charger.battery_current_1_deci * 100,
                (int32_t)d->record.ac_charger.battery_voltage_1_centi);
            break;
        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            /* Orion Tr (0x04): no da corriente, solo estado. Pulso on/off en la
             * serie OrionTR: altura fija mientras carga, 0 cuando no -> se ve
             * cuando y cuanto tiempo carga. El "total Ah" de esta serie sera
             * altura*tiempo, NO amperios-hora reales. */
            uint8_t st = d->record.dcdc.device_state;
            bool charging = (st == VIC_STATE_BULK || st == VIC_STATE_ABSORPTION ||
                             st == VIC_STATE_FLOAT || st == VIC_STATE_STORAGE ||
                             st == VIC_STATE_EQUALIZE || st == VIC_STATE_POWER_SUPPLY);
            battery_history_update_latest(BH_SRC_ORION_XS,
                charging ? ORION_TR_ON_MILLIAMPS : 0, 0);
            break;
        }
        default:
            break;
    }

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
    nvs_commit(h);
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

static void ensure_device_layout(ui_state_t *ui, victron_record_type_t type)
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

static void ui_prepare_detailed_device_status(const victron_data_t *data, char *status_out, size_t status_size)
{
    if (data == NULL || status_out == NULL || status_size == 0) {
        return;
    }

    switch (data->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            const victron_record_battery_monitor_t *batt = &data->record.battery;
            if (batt->soc_deci_percent != 0xFFFF && batt->battery_voltage_centi > 0) {
                uint16_t soc_pct = batt->soc_deci_percent / 10;
                uint16_t soc_dec = batt->soc_deci_percent % 10;
                uint16_t volts = batt->battery_voltage_centi / 100;
                uint16_t hundredths = batt->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u.%u%% | Voltage: %u.%02uV", 
                         soc_pct, soc_dec, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Battery Monitor");
            }
            break;
        }

        case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
            const victron_record_solar_charger_t *solar = &data->record.solar;
            if (solar->pv_power_w > 0 && solar->battery_voltage_centi > 0) {
                uint16_t volts = solar->battery_voltage_centi / 100;
                uint16_t hundredths = solar->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "Power: %uW | Battery: %u.%02uV", 
                         solar->pv_power_w, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Solar Charger");
            }
            break;
        }

        case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
            const victron_record_lynx_smart_bms_t *bms = &data->record.lynx;
            if (bms->soc_deci_percent > 0 && bms->battery_voltage_centi > 0) {
                uint16_t soc_pct = bms->soc_deci_percent / 10;
                uint16_t soc_dec = bms->soc_deci_percent % 10;
                uint16_t volts = bms->battery_voltage_centi / 100;
                uint16_t hundredths = bms->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u.%u%% | Voltage: %u.%02uV", 
                         soc_pct, soc_dec, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Lynx Smart BMS");
            }
            break;
        }

        case VICTRON_BLE_RECORD_INVERTER: {
            const victron_record_inverter_t *inv = &data->record.inverter;
            if (inv->ac_apparent_power_va > 0 && inv->battery_voltage_centi > 0) {
                uint16_t volts = inv->battery_voltage_centi / 100;
                uint16_t hundredths = inv->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "Power: %uVA | Battery: %u.%02uV", 
                         inv->ac_apparent_power_va, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Inverter");
            }
            break;
        }

        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            const victron_record_dcdc_converter_t *dcdc = &data->record.dcdc;
            if (dcdc->input_voltage_centi > 0 && dcdc->output_voltage_centi > 0) {
                uint16_t in_volts = dcdc->input_voltage_centi / 100;
                uint16_t in_hundredths = dcdc->input_voltage_centi % 100;
                uint16_t out_volts = dcdc->output_voltage_centi / 100;
                uint16_t out_hundredths = dcdc->output_voltage_centi % 100;
                snprintf(status_out, status_size, "In: %u.%02uV | Out: %u.%02uV", 
                         in_volts, in_hundredths, out_volts, out_hundredths);
            } else {
                snprintf(status_out, status_size, "Active - DC/DC Converter");
            }
            break;
        }

        case VICTRON_BLE_RECORD_ORION_XS: {
            const victron_record_orion_xs_t *orion = &data->record.orion;
            if (orion->input_voltage_centi > 0 && orion->output_voltage_centi > 0) {
                uint16_t in_volts = orion->input_voltage_centi / 100;
                uint16_t in_hundredths = orion->input_voltage_centi % 100;
                uint16_t out_volts = orion->output_voltage_centi / 100;
                uint16_t out_hundredths = orion->output_voltage_centi % 100;
                snprintf(status_out, status_size, "In: %u.%02uV | Out: %u.%02uV", 
                         in_volts, in_hundredths, out_volts, out_hundredths);
            } else {
                snprintf(status_out, status_size, "Active - Orion XS");
            }
            break;
        }

        case VICTRON_BLE_RECORD_VE_BUS: {
            const victron_record_ve_bus_t *vebus = &data->record.vebus;
            if (vebus->soc_percent > 0 && vebus->battery_voltage_centi > 0) {
                uint16_t volts = vebus->battery_voltage_centi / 100;
                uint16_t hundredths = vebus->battery_voltage_centi % 100;
                snprintf(status_out, status_size, "SOC: %u%% | Battery: %u.%02uV", 
                         vebus->soc_percent, volts, hundredths);
            } else {
                snprintf(status_out, status_size, "Active - VE.Bus System");
            }
            break;
        }

        default:
            snprintf(status_out, status_size, "Active - Device Connected");
            break;
    }
}

static void ui_update_device_activity(ui_state_t *ui, const char *mac_address)
{
    if (ui == NULL || mac_address == NULL) {
        return;
    }
    
    // Get current time in milliseconds
    uint32_t current_time = lv_tick_get();
    
    // Find existing entry or empty slot
    int slot = -1;
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (strcmp(ui->last_active_devices[i], mac_address) == 0) {
            // Found existing entry
            slot = i;
            break;
        }
        if (slot == -1 && ui->last_active_devices[i][0] == '\0') {
            // Found empty slot
            slot = i;
        }
    }
    
    if (slot >= 0) {
        // Update activity record
        strncpy(ui->last_active_devices[slot], mac_address, sizeof(ui->last_active_devices[slot]) - 1);
        ui->last_active_devices[slot][sizeof(ui->last_active_devices[slot]) - 1] = '\0';
        ui->last_activity_time[slot] = current_time;
    }
}

static void ui_check_device_timeouts(lv_timer_t *timer)
{
    ui_state_t *ui = (ui_state_t *)timer->user_data;
    if (ui == NULL) {
        return;
    }
    
    uint32_t current_time = lv_tick_get();
    const uint32_t timeout_ms = 30000; // 30 seconds timeout
    
    // Check each tracked device for timeout
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (ui->last_active_devices[i][0] != '\0') {
            uint32_t time_since_last = current_time - ui->last_activity_time[i];
            
            if (time_since_last > timeout_ms) {
                // Device has timed out - mark as offline
                ui_settings_panel_update_victron_device_status(ui, ui->last_active_devices[i], 
                                                              "", "", "Offline - No data received");
                
                // Clear the tracking entry
                ui->last_active_devices[i][0] = '\0';
                ui->last_activity_time[i] = 0;
            }
        }
    }
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

void ui_mark_device_offline(const char *mac_address)
{
    if (mac_address == NULL) {
        return;
    }
    
    ui_state_t *ui = &g_ui;
    
    // Update device status to offline
    ui_settings_panel_update_victron_device_status(ui, mac_address, "", "", "Offline - Connection lost");
    
    // Remove from activity tracking
    for (int i = 0; i < UI_MAX_VICTRON_DEVICES; i++) {
        if (strcmp(ui->last_active_devices[i], mac_address) == 0) {
            ui->last_active_devices[i][0] = '\0';
            ui->last_activity_time[i] = 0;
            break;
        }
    }
}

void ui_refresh_victron_device_list(void)
{
    ui_state_t *ui = &g_ui;
    ESP_LOGI("ui", "Refreshing Victron device list in settings panel");
    if (lvgl_port_lock(200)) {
        ui_settings_panel_refresh_victron_devices(ui);
        lvgl_port_unlock();
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

/* ── Pantalla gráfica temperaturas ─────────────────────────── */
static lv_obj_t *s_chart_screen = NULL;  /* overlay raíz */
static lv_obj_t *s_chart      = NULL;     /* widget chart interno */
static lv_chart_series_t *s_ser_aletas     = NULL;
static lv_chart_series_t *s_ser_congelador = NULL;
static lv_chart_series_t *s_ser_exterior   = NULL;
static lv_chart_series_t *s_ser_fan        = NULL;
/* Banda del excedente solar: comparte el eje secundario (0..100) con s_ser_fan
 * porque el proyecto fija ese rango una sola vez al crear el chart
 * (lv_chart_set_range en ui_show_chart_screen); no se puede pedir un rango
 * 0..1 propio sin romper la escala del fan%. Se dibuja como marca baja
 * (valor bajo del 0..100) solo en las muestras activas. */
static lv_chart_series_t *s_ser_solar      = NULL;
/* Leyenda-boton: cada elemento muestra/oculta su serie. Guardamos etiqueta,
 * punto y color para poder atenuarlos al ocultar. Estado por sesion de pantalla
 * (las series se recrean al abrir -> todo visible por defecto). */
static lv_obj_t   *s_frigo_leg_lbl[4] = {NULL};
static lv_obj_t   *s_frigo_leg_dot[4] = {NULL};
static lv_color_t  s_frigo_leg_col[4];
static bool        s_frigo_ser_hidden[4] = {false};
static lv_obj_t *s_frigo_lbl_date = NULL;    /* header con la fecha */
static lv_obj_t *s_frigo_xlabels = NULL;     /* contenedor de etiquetas hora */
static lv_obj_t *s_frigo_lbl_zoom = NULL;
static int  s_frigo_day_idx = -1;            /* -1 = "hoy" buffer RAM */
static int  s_frigo_n_dates = 0;
static char s_frigo_dates[LOG_BROWSER_MAX_DATES][LOG_BROWSER_DATE_LEN];
/* Ventana [a, b) en fraccion 0..1 sobre los datos del dia */
static float s_frigo_win_a = 0.0f;
static float s_frigo_win_b = 1.0f;

/* Estado del gesto tactil (arrastrar = pan, doble-toque = zoom, manten = 1x) */
static int32_t s_frigo_drag_last_x  = 0;
static int32_t s_frigo_drag_moved   = 0;
static bool    s_frigo_dragging     = false;
static int64_t s_frigo_last_apply_us = 0;
static int64_t s_frigo_last_click_us = 0;

/* Buffer para parsear el CSV de un dia guardado (1440 muestras = 1 min c/u).
 * En PSRAM (lazy): 1500 x 24 B ~= 36 KB, demasiado para RAM interna estatica.
 * Se reserva al ver un dia historico y se libera al cerrar la grafica. */
#define FRIGO_LOG_MAX_ENTRIES   1500
static frigo_log_entry_t *s_frigo_buf = NULL;
/* Cache: dia (idx) ya parseado en s_frigo_buf y su n. Evita re-leer/re-parsear
 * el CSV de la SD en cada tick de pan/zoom (apply_window). -2 = cache vacia. */
static int s_frigo_loaded_idx = -2;
static int s_frigo_loaded_n   = 0;

static void frigo_chart_load_day(void);
static void frigo_chart_gesture_cb(lv_event_t *e);
static void frigo_arrow_cb(lv_event_t *e);
static void frigo_chart_touch_cb(lv_event_t *e);
static void frigo_legend_toggle_cb(lv_event_t *e);
static void frigo_apply_window(void);
static void frigo_update_zoom_label(void);

/* "YYYY-MM-DD" de hoy. Usado para no listar el CSV de hoy como fecha
 * navegable ademas de "HOY" (idx -1): mismo dia, dos pestanas identicas. */
static void today_ymd(char out[11])
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(out, 11, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static void chart_screen_close_cb(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    lv_obj_del(screen);
    s_chart_screen = NULL;
    s_chart = NULL;
}



/* OJO: el CSV de hoy TIENE que seguir en la lista de dias navegables. Parece
 * redundante con la vista "HOY", pero no lo es: "HOY" sale del buffer de RAM,
 * que arranca VACIO en cada reinicio y solo guarda ~16 h. El fichero de la SD es
 * la unica copia del dia que sobrevive a un arranque. Filtrarlo (v1.4.1) dejo
 * los dos historicos vacios tras instalar una actualizacion. */

void ui_show_chart_screen(ui_state_t *ui)
{
    if (!ui) return;
    /* Guard anti doble-apertura: sin esto un segundo tap deja el overlay
     * anterior huerfano (fuga de pantalla LVGL entera) y los s_* colgantes.
     * Mismo patron que ui_show_battery_history_screen. */
    if (s_chart_screen) return;

    /* Crear pantalla a pantalla completa */
    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_move_foreground(scr);
    s_chart_screen = scr;

    /* Título */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_label_set_text(lbl_title, "Temperaturas");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 16, 12);

    /* Fecha del log mostrado (centrada arriba) */
    s_frigo_lbl_date = lv_label_create(scr);
    lv_obj_set_style_text_font(s_frigo_lbl_date, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_frigo_lbl_date, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_frigo_lbl_date, "HOY");
    lv_obj_align(s_frigo_lbl_date, LV_ALIGN_TOP_MID, 0, 12);

    /* Flechas PULSABLES a izq/dcha de la fecha. Antes eran solo decorativas
     * ("para indicar swipe"): parecian controles y no hacian nada al tocarlas. */
    lv_obj_t *lbl_arr_l = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_l, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_l, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_l, LV_SYMBOL_LEFT);
    lv_obj_align(lbl_arr_l, LV_ALIGN_TOP_MID, -120, 14);
    lv_obj_add_flag(lbl_arr_l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_arr_l, 30);
    lv_obj_add_event_cb(lbl_arr_l, frigo_arrow_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *lbl_arr_r = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_r, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_r, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_r, LV_SYMBOL_RIGHT);
    lv_obj_align(lbl_arr_r, LV_ALIGN_TOP_MID, 120, 14);
    lv_obj_add_flag(lbl_arr_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_arr_r, 30);
    lv_obj_add_event_cb(lbl_arr_r, frigo_arrow_cb, LV_EVENT_CLICKED, NULL);

    /* Boton cerrar */
    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Cerrar");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, chart_screen_close_cb, LV_EVENT_CLICKED, scr);

    /* Leyenda */
    const char *leyenda[] = {"Aletas", "Congel.", "Exter.", "Fan%"};
    lv_color_t colores[]  = {
        lv_color_hex(0x00BFFF),
        lv_color_hex(0xFF4444),
        lv_color_hex(0x44FF44),
        lv_color_hex(0xFFAA00)
    };
    for (int i = 0; i < 4; i++) {
        s_frigo_leg_col[i]    = colores[i];
        s_frigo_ser_hidden[i] = false;

        /* Cada elemento de la leyenda es un boton: al tocarlo se muestra/oculta
         * su linea en la grafica. El contenedor transparente es el area tactil. */
        lv_obj_t *item = lv_obj_create(scr);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, 150, 34);
        /* Grupo centrado: 4 items de paso 165 ocupan (3*165 + 150) px. */
        lv_obj_set_pos(item, (LV_HOR_RES - (3 * 165 + 150)) / 2 + i * 165, 560);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(item, frigo_legend_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(item);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_bg_color(dot, colores[i], 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);  /* el toque lo recibe el item */

        lv_obj_t *lbl = lv_label_create(item);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(lbl, colores[i], 0);
        lv_label_set_text(lbl, leyenda[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 24, 0);

        s_frigo_leg_dot[i] = dot;
        s_frigo_leg_lbl[i] = lbl;
    }

    s_frigo_win_a = 0.0f;
    s_frigo_win_b = 1.0f;

    /* Etiqueta de nivel de zoom (derecha, bajo el boton Cerrar) */
    s_frigo_lbl_zoom = lv_label_create(scr);
    lv_obj_set_style_text_color(s_frigo_lbl_zoom, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_frigo_lbl_zoom, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(s_frigo_lbl_zoom, "1x");
    lv_obj_align(s_frigo_lbl_zoom, LV_ALIGN_TOP_RIGHT, -16, 80);

    /* Pista de uso tactil (zoom/pan sin botones) */
    {
        lv_obj_t *hint = lv_label_create(scr);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8A93A6), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14_es, 0);
        lv_label_set_text(hint,
            "Arrastra: mover  -  2 toques: zoom  -  manten: 1x");
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, 84);
    }

    /* Gráfica */
    s_chart = lv_chart_create(scr);
    /* Estrechado para dejar margen a las etiquetas de los dos ejes Y (LVGL las
     * dibuja fuera del borde del chart): temp a la izquierda, fan% a la derecha. */
    lv_obj_set_size(s_chart, LV_HOR_RES - 150, LV_VER_RES - 186);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart, frigo_chart_touch_cb, LV_EVENT_ALL, NULL);
    /* Sin esto el gesto de deslizar MUERE en la grafica: LVGL lo entrega al
     * objeto tocado y solo sube al padre con EVENT_BUBBLE. Como la grafica ocupa
     * casi toda la pantalla, deslizar donde es natural no cambiaba de dia. */
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(0x333333), 0);
    lv_chart_set_div_line_count(s_chart, 5, 10);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x333333), LV_PART_MAIN);

    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y,   8, 4, 5, 1, true, 60);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_SECONDARY_Y, 8, 4, 5, 1, true, 60);
    lv_obj_set_style_pad_left(s_chart, 8, 0);
    lv_obj_set_style_pad_right(s_chart, 8, 0);
    /* Hueco arriba para que la etiqueta Y superior (fuente 20) no se recorte. */
    lv_obj_set_style_pad_top(s_chart, 16, 0);
    lv_obj_set_style_text_color(s_chart, lv_color_hex(0xAAAAAA), LV_PART_TICKS);
    lv_obj_set_style_text_font(s_chart, &lv_font_montserrat_20_es, LV_PART_TICKS);

    s_ser_aletas     = lv_chart_add_series(s_chart, colores[0], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_congelador = lv_chart_add_series(s_chart, colores[1], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_exterior   = lv_chart_add_series(s_chart, colores[2], LV_CHART_AXIS_PRIMARY_Y);
    s_ser_fan        = lv_chart_add_series(s_chart, colores[3], LV_CHART_AXIS_SECONDARY_Y);
    s_ser_solar      = lv_chart_add_series(s_chart, lv_color_hex(0xE0900A), LV_CHART_AXIS_SECONDARY_Y);

    /* Contenedor de labels horarios bajo el chart */
    s_frigo_xlabels = lv_obj_create(scr);
    lv_obj_remove_style_all(s_frigo_xlabels);
    lv_obj_set_size(s_frigo_xlabels, LV_HOR_RES - 150, 28);
    lv_obj_align_to(s_frigo_xlabels, s_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_layout(s_frigo_xlabels, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_frigo_xlabels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_frigo_xlabels, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_label_create(s_frigo_xlabels);
        lv_obj_set_style_text_color(l, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20_es, 0);
        lv_label_set_text(l, "--:--");
    }

    /* Inicializar navegacion: listar fechas SD y mostrar HOY */
    s_frigo_n_dates = log_browser_list_dates("/sdcard/frigo",
                                             s_frigo_dates, LOG_BROWSER_MAX_DATES);
    if (s_frigo_n_dates > 0) {
        char today[11];
        today_ymd(today);
        if (strcmp(s_frigo_dates[s_frigo_n_dates - 1], today) == 0)
            s_frigo_n_dates--;
    }
    s_frigo_day_idx = -1;
    s_frigo_loaded_idx = -2;   /* re-listado de fechas: invalidar cache del CSV */
    frigo_chart_load_day();

    /* Gestures para navegar entre dias */
    lv_obj_add_event_cb(scr, frigo_chart_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* Toca un elemento de la leyenda -> muestra/oculta su serie y atenua la etiqueta. */
static void frigo_legend_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 4 || !s_chart) return;
    lv_chart_series_t *ser = (i == 0) ? s_ser_aletas
                           : (i == 1) ? s_ser_congelador
                           : (i == 2) ? s_ser_exterior
                                      : s_ser_fan;
    if (!ser) return;
    bool hide = !s_frigo_ser_hidden[i];
    s_frigo_ser_hidden[i] = hide;
    lv_chart_hide_series(s_chart, ser, hide);
    if (s_frigo_leg_lbl[i])
        lv_obj_set_style_text_color(s_frigo_leg_lbl[i],
            hide ? lv_color_hex(0x555555) : s_frigo_leg_col[i], 0);
    if (s_frigo_leg_dot[i])
        lv_obj_set_style_bg_opa(s_frigo_leg_dot[i],
            hide ? LV_OPA_30 : LV_OPA_COVER, 0);
}

/* `base` = indice global de la primera muestra de HOY, `n` = cuantas hay. Los
 * indices que se pasan a datalogger_get_entry siguen siendo globales. */
static void update_frigo_xlabels_today(int base, int n)
{
    if (!s_frigo_xlabels) return;
    if (n <= 0) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
            if (l) lv_label_set_text(l, "--:--");
        }
        return;
    }
    int a = (int)(s_frigo_win_a * n);
    int b = (int)(s_frigo_win_b * n);
    if (b <= a) b = a + 1;
    if (b > n) b = n;
    int wn = b - a;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
        if (!l) continue;
        int idx = base + a + (wn - 1) * i / 4;
        if (idx < base) idx = base;
        if (idx >= base + n) idx = base + n - 1;
        const datalogger_entry_t *e = datalogger_get_entry(idx);
        if (!e) { lv_label_set_text(l, "--:--"); continue; }
        const char *ts = e->timestamp;
        char buf[8] = {0};
        if (strncmp(ts, "BOOT", 4) == 0 && strlen(ts) >= 10) {
            strncpy(buf, ts + 5, 5);
        } else if (strlen(ts) >= 16) {
            strncpy(buf, ts + 11, 5);
        } else {
            buf[0] = 0;
        }
        lv_label_set_text(l, buf[0] ? buf : "--:--");
    }
}

static void update_frigo_xlabels_from_buf(int n)
{
    if (!s_frigo_xlabels) return;
    if (n <= 0) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
            if (l) lv_label_set_text(l, "--:--");
        }
        return;
    }
    int a = (int)(s_frigo_win_a * n);
    int b = (int)(s_frigo_win_b * n);
    if (b <= a) b = a + 1;
    if (b > n) b = n;
    int wn = b - a;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_frigo_xlabels, i);
        if (!l) continue;
        int idx = a + (wn - 1) * i / 4;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        lv_label_set_text_fmt(l, "%02d:%02d",
                              s_frigo_buf[idx].hh, s_frigo_buf[idx].mm);
    }
}

static void frigo_apply_temp_range(float t_min, float t_max)
{
    if (t_min > t_max) { t_min = -20.0f; t_max = 15.0f; }
    float span = t_max - t_min;
    if (span < 1.0f) span = 1.0f;
    int y_min = (int)(t_min - span * 0.05f);
    int y_max = (int)(t_max + span * 0.05f) + 1;
    if (y_min == y_max) { y_min--; y_max++; }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);
}

/* Indice global de la primera muestra de HOY en el buffer del datalogger. Las
 * muestras estan en orden cronologico, asi que las de hoy son el tramo final.
 * Devuelve `count` si no hay ninguna de hoy, y 0 si el reloj aun no tiene hora
 * (entonces se muestra todo, que es lo unico util sin fecha). */
static int frigo_today_base(int count)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year <= 100) return 0;
    char today[11];
    snprintf(today, sizeof today, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    for (int i = 0; i < count; ++i) {
        const datalogger_entry_t *e = datalogger_get_entry(i);
        /* NULL = no se consiguio el mutex del datalogger (hay un volcado a la SD
         * en curso), NO "esta muestra no es de hoy". Tratarlo como descarte
         * vaciaba la grafica entera si el volcado pillaba el barrido. Ante la
         * duda, mostrar todo el buffer: es el comportamiento de siempre. */
        if (!e) return 0;
        if (strncmp(e->timestamp, today, 10) == 0) return i;
    }
    return count;
}

/* "YYYY-MM-DD" -> "DD-MM-YYYY", solo para mostrar. El string de origen (nombre
 * de fichero, orden alfabetico ascendente) no se toca. */
static void fmt_date_ddmmaaaa(const char *iso, char *out, size_t out_len)
{
    if (!iso || strlen(iso) != 10) { snprintf(out, out_len, "%s", iso ? iso : ""); return; }
    snprintf(out, out_len, "%.2s-%.2s-%.4s", iso + 8, iso + 5, iso);
}

/* Pega al final de `buf` (que ya trae el CSV de hoy) las muestras del anillo
 * del datalogger posteriores a la ultima que trajo el CSV, y actualiza `n`.
 *
 * Mismo motivo que bh_append_ring_tail (bateria, mas abajo): el CSV llega
 * hasta el ultimo volcado (cada 60 s) pero SOBREVIVE a los reinicios, y el
 * anillo tiene el minuto en curso pero arranca vacio en cada arranque. Juntos
 * dan el dia entero; solo el CSV o solo el anillo dejan el dia partido en dos
 * vistas ("HOY" con lo de despues del reinicio, la fecha de hoy navegable con
 * lo de antes). */
static void frigo_append_ring_tail(frigo_log_entry_t *buf, int *n, int max)
{
    char today[11];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(today, sizeof today, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);

    int last_min = (*n > 0) ? buf[*n - 1].hh * 60 + buf[*n - 1].mm : -1;
    int count = datalogger_get_count();
    for (int i = 0; i < count && *n < max; ++i) {
        const datalogger_entry_t *e = datalogger_get_entry(i);
        if (!e) continue;
        if (strncmp(e->timestamp, today, 10) != 0) continue;   /* no es de hoy */
        int hh = 0, mm = 0;
        if (sscanf(e->timestamp + 11, "%d:%d", &hh, &mm) != 2) continue;
        if (hh * 60 + mm <= last_min) continue;   /* ya venia en el CSV */
        frigo_log_entry_t *o = &buf[*n];
        o->hh = hh;
        o->mm = mm;
        o->t_aletas = e->T_Aletas;
        o->t_congel = e->T_Congelador;
        o->t_exter  = e->T_Exterior;
        o->fan_pct  = e->fan_percent;
        o->excedente_solar = e->excedente_solar ? 1 : 0;
        (*n)++;
    }
}

static void frigo_chart_load_day(void)
{
    if (!s_chart) return;
    /* Resetear todas las series e indice circular */
    lv_chart_set_all_value(s_chart, s_ser_aletas,     LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_congelador, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_exterior,   LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_fan,        LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_solar,      LV_CHART_POINT_NONE);

    time_t frigo_now = time(NULL);
    struct tm frigo_lt;
    localtime_r(&frigo_now, &frigo_lt);
    bool frigo_clock_ok = frigo_lt.tm_year > 100;

    if (s_frigo_day_idx < 0 && !frigo_clock_ok) {
        /* Reloj sin hora aun: no hay como nombrar el CSV de hoy ni fecharlo
         * (BOOT+HH:MM:SS en vez de fecha real). Mostrar el anillo entero sin
         * fusionar con la SD, que es lo unico util sin fecha. */
        int count = datalogger_get_count();
        int base  = frigo_today_base(count);
        int n     = count - base;
        if (n < 2) { n = 2; base = count - 2; if (base < 0) base = 0; }
        int wa = base + (int)(s_frigo_win_a * n);
        int wb = base + (int)(s_frigo_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > base + n) wb = base + n;
        int wn = wb - wa;
        if (wn < 2) wn = 2;
        lv_chart_set_point_count(s_chart, wn);
        float t_min = 9999.0f, t_max = -9999.0f;
        int valid = 0;
        for (int i = wa; i < wb; i++) {
            const datalogger_entry_t *e = datalogger_get_entry(i);
            if (!e) continue;
            valid++;
            int idx = i - wa;
            if (e->T_Aletas     > -120.0f) { if (e->T_Aletas     < t_min) t_min = e->T_Aletas;     if (e->T_Aletas     > t_max) t_max = e->T_Aletas; }
            if (e->T_Congelador > -120.0f) { if (e->T_Congelador < t_min) t_min = e->T_Congelador; if (e->T_Congelador > t_max) t_max = e->T_Congelador; }
            if (e->T_Exterior   > -120.0f) { if (e->T_Exterior   < t_min) t_min = e->T_Exterior;   if (e->T_Exterior   > t_max) t_max = e->T_Exterior; }
            lv_chart_set_value_by_id(s_chart, s_ser_aletas, idx,
                e->T_Aletas > -120.0f ? (int16_t)e->T_Aletas : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_congelador, idx,
                e->T_Congelador > -120.0f ? (int16_t)e->T_Congelador : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_exterior, idx,
                e->T_Exterior > -120.0f ? (int16_t)e->T_Exterior : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_fan, idx, e->fan_percent);
            lv_chart_set_value_by_id(s_chart, s_ser_solar, idx,
                e->excedente_solar ? 3 : LV_CHART_POINT_NONE);
        }
        frigo_apply_temp_range(t_min, t_max);
        /* Pasamos el rango raw del datalogger (base + n), no `valid`: las
         * funciones de xlabels indexan en datalogger_get_entry(idx) que
         * usa el espacio global de indices; pasar `valid` desincroniza
         * los timestamps del rango visible (zoom al 50-100% mostraba
         * etiquetas del 0-50%). El "--:--" ocasional con datalogger
         * vacio es un bug menor que las etiquetas con la hora incorrecta. */
        update_frigo_xlabels_today(base, n);
        if (s_frigo_lbl_date) lv_label_set_text(s_frigo_lbl_date, "HOY");
        (void)valid;
    } else {
        /* HOY (idx<0, reloj en hora) fusiona el CSV de hoy con la cola del
         * anillo, igual que hace la bateria (bh_loader_task, mas abajo): el
         * CSV sobrevive a los reinicios pero solo llega hasta el ultimo
         * volcado (cada 60 s), y el anillo tiene el minuto en curso pero
         * arranca vacio en cada arranque. Sin esto, HOY solo ensenaba lo de
         * despues del ultimo reinicio y el resto del dia solo se veia
         * navegando a la fecha de hoy como "historico". */
        char date[LOG_BROWSER_DATE_LEN];
        if (s_frigo_day_idx < 0) {
            snprintf(date, sizeof date, "%04d-%02d-%02d",
                     frigo_lt.tm_year + 1900, frigo_lt.tm_mon + 1, frigo_lt.tm_mday);
        } else {
            snprintf(date, sizeof date, "%s", s_frigo_dates[s_frigo_day_idx]);
        }
        if (s_frigo_buf == NULL) {
            s_frigo_buf = heap_caps_malloc(sizeof(frigo_log_entry_t) * FRIGO_LOG_MAX_ENTRIES,
                                           MALLOC_CAP_SPIRAM);
            s_frigo_loaded_idx = -2;   /* buffer nuevo: cache invalida */
        }
        int n;
        if (s_frigo_buf && s_frigo_day_idx == s_frigo_loaded_idx) {
            n = s_frigo_loaded_n;   /* mismo dia ya en s_frigo_buf: no re-leer la SD */
        } else {
            char path[64];
            snprintf(path, sizeof(path), "/sdcard/frigo/%s.csv", date);
            n = s_frigo_buf ? log_browser_load_frigo(path, s_frigo_buf, FRIGO_LOG_MAX_ENTRIES) : 0;
            if (s_frigo_day_idx < 0 && s_frigo_buf)
                frigo_append_ring_tail(s_frigo_buf, &n, FRIGO_LOG_MAX_ENTRIES);
            s_frigo_loaded_idx = s_frigo_buf ? s_frigo_day_idx : -2;
            s_frigo_loaded_n   = n;
        }
        int wa = (int)(s_frigo_win_a * n);
        int wb = (int)(s_frigo_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > n) wb = n;
        int wn = wb - wa;
        /* Mismo tope y downsample que la grafica de bateria (ver
         * ui_show_battery_history_screen): con varias series, un
         * lv_chart_set_point_count grande cuelga taskLVGL > 5 s -> WDT. Aqui hay
         * 5 series y el buffer admite hasta FRIGO_LOG_MAX_ENTRIES (1500); un CSV
         * real son ~288 lineas (log cada 5 min), asi que hoy no se alcanza, pero
         * el tope evita que un cambio de cadencia lo reviva. */
        const int CHART_MAX_PTS = 300;
        int pts = wn > 0 ? wn : 2;
        if (pts > CHART_MAX_PTS) pts = CHART_MAX_PTS;
        if (pts < 2) pts = 2;
        lv_chart_set_point_count(s_chart, pts);
        int step = (wn > CHART_MAX_PTS) ? (wn + CHART_MAX_PTS - 1) / CHART_MAX_PTS : 1;
        float t_min = 9999.0f, t_max = -9999.0f;
        int idx = 0;
        for (int i = wa; i < wb && idx < pts; i += step, ++idx) {
            const frigo_log_entry_t *e = &s_frigo_buf[i];
            if (!isnan(e->t_aletas))  { if (e->t_aletas  < t_min) t_min = e->t_aletas;  if (e->t_aletas  > t_max) t_max = e->t_aletas; }
            if (!isnan(e->t_congel))  { if (e->t_congel  < t_min) t_min = e->t_congel;  if (e->t_congel  > t_max) t_max = e->t_congel; }
            if (!isnan(e->t_exter))   { if (e->t_exter   < t_min) t_min = e->t_exter;   if (e->t_exter   > t_max) t_max = e->t_exter; }
            lv_chart_set_value_by_id(s_chart, s_ser_aletas, idx,
                isnan(e->t_aletas) ? LV_CHART_POINT_NONE : (int16_t)e->t_aletas);
            lv_chart_set_value_by_id(s_chart, s_ser_congelador, idx,
                isnan(e->t_congel) ? LV_CHART_POINT_NONE : (int16_t)e->t_congel);
            lv_chart_set_value_by_id(s_chart, s_ser_exterior, idx,
                isnan(e->t_exter)  ? LV_CHART_POINT_NONE : (int16_t)e->t_exter);
            lv_chart_set_value_by_id(s_chart, s_ser_fan, idx, e->fan_pct);
            lv_chart_set_value_by_id(s_chart, s_ser_solar, idx,
                e->excedente_solar ? 3 : LV_CHART_POINT_NONE);
        }
        frigo_apply_temp_range(t_min, t_max);
        update_frigo_xlabels_from_buf(n);
        if (s_frigo_lbl_date) {
            if (s_frigo_day_idx < 0) {
                lv_label_set_text(s_frigo_lbl_date, "HOY");
            } else {
                char disp[11];
                fmt_date_ddmmaaaa(date, disp, sizeof disp);
                lv_label_set_text(s_frigo_lbl_date, disp);
            }
        }
    }
    lv_chart_refresh(s_chart);
}

static void frigo_update_zoom_label(void)
{
    if (!s_frigo_lbl_zoom) return;
    float w = s_frigo_win_b - s_frigo_win_a;
    if (w <= 0.0f) w = 1.0f;
    float z = 1.0f / w;
    if (z < 1.05f) lv_label_set_text(s_frigo_lbl_zoom, "1x");
    else if (z < 9.5f) lv_label_set_text_fmt(s_frigo_lbl_zoom, "%.1fx", z);
    else               lv_label_set_text_fmt(s_frigo_lbl_zoom, "%dx", (int)(z + 0.5f));
}

static void frigo_apply_window(void)
{
    if (s_frigo_win_a < 0.0f) s_frigo_win_a = 0.0f;
    if (s_frigo_win_b > 1.0f) s_frigo_win_b = 1.0f;
    if (s_frigo_win_b - s_frigo_win_a < 0.01f) {
        float c = (s_frigo_win_a + s_frigo_win_b) * 0.5f;
        s_frigo_win_a = c - 0.005f;
        s_frigo_win_b = c + 0.005f;
        if (s_frigo_win_a < 0) { s_frigo_win_b -= s_frigo_win_a; s_frigo_win_a = 0; }
        if (s_frigo_win_b > 1) { s_frigo_win_a -= (s_frigo_win_b - 1); s_frigo_win_b = 1; }
    }
    frigo_update_zoom_label();
    frigo_chart_load_day();
}

/* Gesto tactil sobre el chart: arrastrar = desplazar (pan), doble-toque =
 * zoom in centrado en el punto, mantener pulsado = volver a 1x. El cambio de
 * dia se hace con swipe (gesture_cb) solo cuando estamos en 1x. */
static void frigo_chart_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_frigo_drag_last_x = p.x;
        s_frigo_drag_moved  = 0;
        s_frigo_dragging    = false;
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dx = p.x - s_frigo_drag_last_x;
        s_frigo_drag_last_x = p.x;
        s_frigo_drag_moved += dx < 0 ? -dx : dx;
        if (s_frigo_drag_moved < 6) return;   /* umbral para no confundir tap */
        s_frigo_dragging = true;
        float win_w = s_frigo_win_b - s_frigo_win_a;
        lv_area_t ca; lv_obj_get_content_coords(s_chart, &ca);
        int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
        float shift = -(float)dx / (float)cw * win_w;
        s_frigo_win_a += shift; s_frigo_win_b += shift;
        if (s_frigo_win_a < 0.0f) { s_frigo_win_b -= s_frigo_win_a; s_frigo_win_a = 0.0f; }
        if (s_frigo_win_b > 1.0f) { s_frigo_win_a -= (s_frigo_win_b - 1.0f); s_frigo_win_b = 1.0f; }
        int64_t now = esp_timer_get_time();
        if (now - s_frigo_last_apply_us > 120000) {  /* throttle recarga ~8/s */
            s_frigo_last_apply_us = now;
            frigo_update_zoom_label();
            frigo_chart_load_day();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_frigo_dragging) {
            frigo_update_zoom_label();
            frigo_chart_load_day();           /* recarga final tras soltar */
            s_frigo_dragging = false;
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (!s_frigo_dragging) {              /* mantener pulsado = reset a 1x */
            s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
            frigo_apply_window();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_frigo_dragging) return;
        int64_t now = esp_timer_get_time();
        if (now - s_frigo_last_click_us < 350000) {   /* doble-toque = zoom in */
            s_frigo_last_click_us = 0;
            float win_w = s_frigo_win_b - s_frigo_win_a;
            lv_area_t ca; lv_obj_get_content_coords(s_chart, &ca);
            int cw = ca.x2 - ca.x1 + 1; if (cw < 1) cw = 1;
            float rel = (float)(p.x - ca.x1) / (float)cw;
            if (rel < 0) rel = 0;
            if (rel > 1) rel = 1;
            float center = s_frigo_win_a + rel * win_w;
            float nw = win_w * 0.5f;
            s_frigo_win_a = center - rel * nw;
            s_frigo_win_b = s_frigo_win_a + nw;
            frigo_apply_window();
        } else {
            s_frigo_last_click_us = now;
        }
    }
}

/* Un dia adelante o atras. `atras` = hacia el pasado. Lo comparten el gesto y
 * los botones de flecha para que no puedan divergir. */
static void frigo_step_day(bool atras)
{
    if (atras) {
        if (s_frigo_day_idx == -1) {
            if (s_frigo_n_dates > 0) s_frigo_day_idx = s_frigo_n_dates - 1;
            else return;
        } else if (s_frigo_day_idx > 0) {
            s_frigo_day_idx--;
        } else {
            return;
        }
    } else {
        if (s_frigo_day_idx < 0) return;
        if (s_frigo_day_idx < s_frigo_n_dates - 1) s_frigo_day_idx++;
        else                                       s_frigo_day_idx = -1;
    }
    s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
    /* Volver a HOY relee: su CSV y el anillo han seguido creciendo mientras
     * se miraban otros dias. Los dias pasados no cambian, asi que esos si
     * valen tal cual desde el buffer (misma logica que bh_step_day). */
    if (s_frigo_day_idx < 0) s_frigo_loaded_idx = -2;
    frigo_update_zoom_label();
    frigo_chart_load_day();
}

static void frigo_arrow_cb(lv_event_t *e)
{
    frigo_step_day(lv_event_get_user_data(e) != NULL);
}

static void frigo_chart_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    /* Zoomeado: el pan se hace arrastrando (frigo_chart_touch_cb); ignoramos
     * el swipe para no cambiar de dia sin querer. */
    bool zoomed = (s_frigo_win_a > 0.0001f) || (s_frigo_win_b < 0.9999f);
    if (zoomed) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT)     frigo_step_day(true);
    else if (dir == LV_DIR_LEFT) frigo_step_day(false);
}

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

/* Cerrar overlays para rotacion del salvapantallas */
void ui_close_chart_screen(void)
{
    /* Borrar el overlay raíz, que arrastra al chart y todos sus hijos */
    if (s_chart_screen) { lv_obj_del(s_chart_screen); s_chart_screen = NULL; }
    s_chart = NULL;
    /* Liberar el buffer de carga de dias guardados (~36KB PSRAM): se reserva
     * lazy al ver un dia y se re-reserva al volver a abrir. */
    if (s_frigo_buf) { free(s_frigo_buf); s_frigo_buf = NULL; s_frigo_loaded_idx = -2; }
}

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

void ui_update_wifi_ssid(ui_state_t *ui)
{
    if (!ui || !ui->wifi.ssid) return;
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        char ssid[20];
        snprintf(ssid, sizeof(ssid), "ESP_%02X%02X%02X", mac[3], mac[4], mac[5]);
        if (lvgl_port_lock(50)) {
            lv_textarea_set_text(ui->wifi.ssid, ssid);
            lvgl_port_unlock();
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * Navegacion de pantallas para captura (carrusel a demanda de Settings y
 * /captura por WiFi). LVGL no es thread-safe: se toma lvgl_port_lock y se
 * suelta durante las esperas para que lleguen datos BLE reales y se dibuje
 * la vista antes de fotografiarla.
 * ────────────────────────────────────────────────────────────────────── */
#define TOUR_DIR        "/sdcard/screenshots"
#define TOUR_SETTLE_MS        1500   /* dejar que la vista se actualice/dibuje */

static void tour_set_view(ui_state_t *ui, ui_view_mode_t mode)
{
    if (lvgl_port_lock(1000)) {
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);  /* Live */
        ui->view_selection.mode = mode;
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }
}

/* Espera a que la vista se dibuje MANTENIENDO la pantalla despierta. La
 * navegacion programatica del tour NO cuenta como actividad del usuario, asi
 * que sin esto, pasados 60 s, saltarian el auto-return de Ajustes (l.868), el
 * idle-to-live o el salvapantallas (que cambiaria de vista) y arruinarian las
 * capturas. Reseteamos los tres relojes de inactividad en cada paso; si el
 * salvapantallas ya estuviera activo, ui_notify_user_activity lo despierta. */
static void tour_settle(void)
{
    if (lvgl_port_lock(500)) {
        lv_disp_trig_activity(NULL);   /* auto-return de Ajustes (inactive_time) */
        ui_notify_user_activity();     /* idle-to-live + screensaver_wake */
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(TOUR_SETTLE_MS));
}

/* ─── Carrusel de captura a demanda (switch en Settings→Display) ──────────────
 * Recorre SOLO las 8 pantallas de datos (6 device-views + grafico bateria +
 * grafico frigo), captura cada una a BMP en la SD (sobrescribe mismos nombres),
 * y al terminar apaga el switch y muestra el resultado. Reutiliza
 * tour_set_view/tour_settle/screenshot_save_bmp. A diferencia de
 * screenshot_tour_task: sin marcador, sin retardo de 60 s y sin las paginas de
 * Ajustes. Un unico disparo a la vez (s_capture_running). */
static volatile bool s_capture_running = false;

/* Guarda una pantalla contando aciertos y recordando el PRIMER error (para
 * diagnostico sin serie: distingue "sin PSRAM" de "SD ocupada/error"). */
static void cap_save(const char *path, int *ok, esp_err_t *first_err)
{
    esp_err_t e = screenshot_save_jpeg(path);
    if (e == ESP_OK) (*ok)++;
    else if (*first_err == ESP_OK) *first_err = e;
}

/* Apaga el switch y refleja el resultado (bajo lock LVGL; valida los objetos
 * por si la pagina Display se hubiera reconstruido durante la captura). */
static void capture_carousel_finish(ui_state_t *ui, int ok, esp_err_t first_err)
{
    if (lvgl_port_lock(1000)) {
        if (ui->capture_switch && lv_obj_is_valid(ui->capture_switch)) {
            lv_obj_clear_state(ui->capture_switch, LV_STATE_CHECKED);
        }
        if (ui->capture_status_lbl && lv_obj_is_valid(ui->capture_status_lbl)) {
            if (ok >= 8) {
                lv_label_set_text(ui->capture_status_lbl,
                                  "8/8 capturas guardadas en la SD");
            } else {
                const char *why = (first_err == ESP_ERR_NO_MEM) ? "sin PSRAM"
                                : (first_err == ESP_FAIL)        ? screenshot_last_error()
                                :                                  "error";
                lv_label_set_text_fmt(ui->capture_status_lbl,
                                      "%d/8 - fallo: %s", ok, why);
            }
        }
        lvgl_port_unlock();
    }
}

static void capture_carousel_task(void *arg)
{
    ui_state_t *ui = &g_ui;
    const ui_view_mode_t saved_mode = ui->view_selection.mode;
    int ok = 0;
    esp_err_t first_err = ESP_OK;
    char path[96];

    const int total = ui_tour_screen_count();
    ESP_LOGI("CAPCAR", "Carrusel de captura: %d pantallas -> %s", total, TOUR_DIR);

    /* UN SOLO bucle sobre ui_tour_goto_screen: es la MISMA lista que usa la
     * pagina web /capturas, asi que una pantalla nueva en el tour entra aqui
     * sola. Antes habia una lista propia de 8 que se habia quedado corta: se
     * dejaba fuera el historico solar, la galeria, los 3 detalles de tarjeta y
     * TODAS las paginas de ajustes. El numero delante del nombre mantiene el
     * orden al listarlas. screenshot_save_jpeg ya crea el directorio padre bajo
     * camera_sd_bus_lock, no hace falta mkdir. 2026-07-26. */
    for (int i = 0; i < total; ++i) {
        const char *name = ui_tour_goto_screen(i);   /* ya hace tour_settle() */
        if (!name) continue;
        snprintf(path, sizeof(path), TOUR_DIR "/%02d_%s.jpg", i, name);
        cap_save(path, &ok, &first_err);
    }

    /* Restaurar: cerrar lo que quede abierto y volver a Live + la vista previa. */
    if (lvgl_port_lock(1000)) {
        ui_close_chart_screen();
        ui_close_battery_history_screen();
        ui_close_solar_history_screen();
        ui_gallery_close();
        ui_close_card_detail();
        ui->view_selection.mode = saved_mode;
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }

    ESP_LOGI("CAPCAR", "Carrusel terminado: %d/%d capturas (first_err=0x%x)",
             ok, total, (int)first_err);
    capture_carousel_finish(ui, ok, first_err);
    s_capture_running = false;
    vTaskDelete(NULL);
}

bool ui_capture_carousel_running(void)
{
    return s_capture_running;
}

void ui_start_capture_carousel(void)
{
    if (s_capture_running) {
        ESP_LOGW("CAPCAR", "Carrusel ya en curso, ignoro");
        return;
    }
    s_capture_running = true;
    if (xTaskCreate(capture_carousel_task, "cap_carousel", 12288, NULL, 3, NULL) != pdPASS) {
        s_capture_running = false;
        ESP_LOGE("CAPCAR", "No pude crear la tarea del carrusel");
    }
}

/* --- Navegacion por indice para las capturas por WiFi ---
 * Mapea un indice 0..(N-1) a una pantalla concreta, navega hasta ella (cerrando
 * antes cualquier overlay para partir de un estado limpio) y espera a que se
 * dibuje con tour_settle(). Reutiliza la misma logica que el auto-tour. Se
 * llama desde el handler HTTP /captura?n=<i> (config_server.c), que luego hace
 * screenshot_take_bmp() y devuelve el BMP. Devuelve el nombre corto de la
 * pantalla (para el nombre de fichero) o NULL si el indice esta fuera de rango. */
static const struct { ui_view_mode_t mode; const char *name; } TOUR_LIVE[] = {
    { UI_VIEW_MODE_OVERVIEW,        "overview"        },
    { UI_VIEW_MODE_DEFAULT_BATTERY, "bateria"         },
    { UI_VIEW_MODE_SOLAR_CHARGER,   "solar"           },
    { UI_VIEW_MODE_BATTERY_MONITOR, "monitor_bateria" },
    { UI_VIEW_MODE_INVERTER,        "inversor"        },
    { UI_VIEW_MODE_DCDC_CONVERTER,  "dcdc"            },
};
/* Detalle de tarjeta: solo estas 3 categorias pintan algo (ver el switch de
 * ui_show_card_detail); el resto cae al default y saldria vacio. */
static const struct { victron_record_type_t cat; const char *name; } TOUR_CARD[] = {
    { VICTRON_BLE_RECORD_SOLAR_CHARGER,   "detalle_solar"   },
    { VICTRON_BLE_RECORD_BATTERY_MONITOR, "detalle_bateria" },
    { VICTRON_BLE_RECORD_DCDC_CONVERTER,  "detalle_dcdc"    },
};
/* MISMO ORDEN que las llamadas a settings_menu_add_entry() en settings_panel.c:
 * el indice del tour es el orden de registro en s_page_ctxs, no el visual.
 * Faltaban "Tarjeta SD" y "Autocaravana", y eso no dejaba dos capturas sin
 * nombre: DESPLAZABA todas las de detras, asi que la pagina de Tarjeta SD se
 * guardaba como "sonido", la de Sonido como "victron_keys", etc. Si se anade una
 * entrada al menu, hay que anadirla AQUI en su sitio. 2026-07-26. */
static const char *TOUR_SET_NAMES[] = {
    "frigo",         /* Opciones Frigo        */
    "logs",          /* Historial en graficos */
    "wifi",          /* Wi-Fi                 */
    "display",       /* Pantalla              */
    "tarjeta_sd",    /* Tarjeta SD            */
    "sonido",        /* Sonido y alertas      */
    "autocaravana",  /* Autocaravana          */
    "victron_keys",  /* Victron Keys          */
    "about",         /* Acerca de             */
};
#define TOUR_N_LIVE   ((int)(sizeof(TOUR_LIVE) / sizeof(TOUR_LIVE[0])))
#define TOUR_N_CARD   ((int)(sizeof(TOUR_CARD) / sizeof(TOUR_CARD[0])))
#define TOUR_I_BATLOG  (TOUR_N_LIVE)              /* 6  */
#define TOUR_I_FRIGLOG (TOUR_N_LIVE + 1)          /* 7  */
#define TOUR_I_SOLLOG  (TOUR_N_LIVE + 2)          /* 8  historico solar */
#define TOUR_I_GALLERY (TOUR_N_LIVE + 3)          /* 9  galeria */
#define TOUR_I_CARD0   (TOUR_N_LIVE + 4)          /* 10..12 detalles de tarjeta */
#define TOUR_I_SETMAIN (TOUR_I_CARD0 + TOUR_N_CARD)  /* 13 */
#define TOUR_I_SETSUB0 (TOUR_I_SETMAIN + 1)          /* 14 */

int ui_tour_screen_count(void)
{
    return TOUR_I_SETSUB0 + ui_settings_panel_page_count();
}

const char *ui_tour_goto_screen(int idx)
{
    ui_state_t *ui = &g_ui;
    if (idx < 0 || idx >= ui_tour_screen_count()) return NULL;

    /* Partir siempre de estado limpio: cerrar overlays abiertos. */
    if (lvgl_port_lock(1000)) {
        ui_close_chart_screen();
        ui_close_battery_history_screen();
        ui_close_solar_history_screen();
        ui_gallery_close();
        ui_close_card_detail();
        lvgl_port_unlock();
    }

    const char *name = "pantalla";
    if (idx < TOUR_N_LIVE) {
        tour_set_view(ui, TOUR_LIVE[idx].mode);
        name = TOUR_LIVE[idx].name;
    } else if (idx == TOUR_I_BATLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_battery_history_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_bateria";
    } else if (idx == TOUR_I_FRIGLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_chart_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_frigo";
    } else if (idx == TOUR_I_SOLLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_solar_history_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_solar";
    } else if (idx == TOUR_I_GALLERY) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_gallery_open();
            lvgl_port_unlock();
        }
        name = "galeria";
    } else if (idx >= TOUR_I_CARD0 && idx < TOUR_I_CARD0 + TOUR_N_CARD) {
        int c = idx - TOUR_I_CARD0;
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_card_detail(ui, TOUR_CARD[c].cat);
            lvgl_port_unlock();
        }
        name = TOUR_CARD[c].name;
    } else if (idx == TOUR_I_SETMAIN) {
        if (lvgl_port_lock(1000)) {
            ui_settings_panel_go_to_main();
            lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
            lvgl_port_unlock();
        }
        name = "ajustes";
    } else {
        int s = idx - TOUR_I_SETSUB0;
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
            ui_settings_panel_show_page(s);
            lvgl_port_unlock();
        }
        name = (s < (int)(sizeof(TOUR_SET_NAMES) / sizeof(TOUR_SET_NAMES[0])))
                   ? TOUR_SET_NAMES[s] : "ajustes";
    }

    tour_settle();
    return name;
}
