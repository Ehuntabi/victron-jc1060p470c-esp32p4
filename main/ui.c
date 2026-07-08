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
#include "pzem004t.h"
#include "log_browser.h"
#include "health_score.h"
#include "screenshot.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "esp_timer.h"

static int64_t s_last_ble_data_us = 0;
static void ble_indicator_timer_cb(lv_timer_t *t);


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
    uint8_t dummy;
    while (1) {
        if (xQueueReceive(s_beep_queue, &dummy, portMAX_DELAY) == pdTRUE) {
            audio_play_jingle(AUDIO_JINGLE_CONFIRM);
        }
    }
}

/* Beep corto al pulsar cualquier widget clicable. Encola y vuelve
 * inmediatamente (no bloquea el thread LVGL). */
static void ui_global_click_beep_cb(lv_event_t *e)
{
    (void)e;
    if (!s_beep_queue) return;
    uint8_t v = 1;
    /* timeout 0 → si la cola esta llena (beep en curso), se descarta el
     * nuevo beep. Evita acumulacion de beeps por clicks rapidos. */
    xQueueSend(s_beep_queue, &v, 0);
}

static void health_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_health) return;
    char reason[28];
    health_level_t lvl = health_score_evaluate(reason, sizeof(reason));
    uint32_t bg, fg;
    const char *txt;
    if (lvl == HEALTH_ALARM) {
        bg = 0xCC3333; fg = 0xFFFFFF; txt = reason;
    } else if (lvl == HEALTH_WARN) {
        bg = 0xFF9800; fg = 0x2A2A2A; txt = reason;
    } else {
        bg = 0x00C851; fg = 0xFFFFFF; txt = "OK";
    }
    lv_obj_set_style_bg_color(ui->lbl_health, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(ui->lbl_health, lv_color_hex(fg), 0);
    /* Ajustar ancho al texto cuando hay motivo (puede ser mas largo que "OK"). */
    lv_obj_set_width(ui->lbl_health,
                     (lvl == HEALTH_OK) ? 60 : LV_SIZE_CONTENT);
    lv_label_set_text(ui->lbl_health, txt);
}

static void ac_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    if (!ui || !ui->lbl_ac) return;
    pzem_data_t pz;
    pzem_get(&pz);
    if (!pz.has_data) {
        lv_obj_add_flag(ui->lbl_ac, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(ui->lbl_ac, LV_OBJ_FLAG_HIDDEN);
    char buf[24];
    if (pz.voltage_v >= 50.0f) {
        /* AC presente: muestra potencia activa */
        snprintf(buf, sizeof(buf), "AC %dW", (int)(pz.power_w + 0.5f));
        lv_label_set_text(ui->lbl_ac, buf);
        lv_obj_set_style_text_color(ui->lbl_ac,
            pz.alarm ? lv_color_hex(0xFF4444) : lv_color_hex(0x00C851), 0);
    } else {
        /* PZEM conectado pero sin tension AC */
        lv_label_set_text(ui->lbl_ac, "AC --");
        lv_obj_set_style_text_color(ui->lbl_ac, lv_color_hex(0x888888), 0);
    }
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
     * ocupado (GDMA camara/esp_hosted) y pasaba de 300ms. Es SEGURO cachear:
     * cualquier cambio del flag 'wifi/enabled' reinicia la placa (siempre pasa por
     * dialogo de reinicio -> esp_restart), asi que el valor del arranque es valido
     * toda la sesion. */
    if (ui->lbl_wifi) {
        static int last_en = -1;
        static int cached_en = -1;
        if (cached_en < 0) {
            nvs_handle_t h;
            uint8_t en = 1;
            if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
                nvs_get_u8(h, "enabled", &en);
                nvs_close(h);
            }
            cached_en = en;
        }
        if (cached_en != last_en) {
            lv_obj_set_style_text_color(ui->lbl_wifi,
                cached_en ? lv_color_hex(0x4FC3F7) : lv_color_hex(0x666666), 0);
            last_en = cached_en;
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
    if (wifi_err != ESP_OK) {
        strncpy(default_ssid, "VictronConfig", sizeof(default_ssid));
        default_ssid[sizeof(default_ssid) - 1] = '\0';
        strncpy(default_pass, DEFAULT_AP_PASSWORD, sizeof(default_pass));
        default_pass[sizeof(default_pass) - 1] = '\0';
        ap_enabled = 1;
        save_wifi_config(default_ssid, default_pass, ap_enabled);
    } else if (default_pass[0] == '\0') {
        strncpy(default_pass, DEFAULT_AP_PASSWORD, sizeof(default_pass));
        default_pass[sizeof(default_pass) - 1] = '\0';
        save_wifi_config(default_ssid, default_pass, ap_enabled);
    }


    load_screensaver_settings(&ui->screensaver.enabled,
                              &ui->screensaver.brightness,
                              &ui->screensaver.timeout);
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
    /* Health score: pill compacto (verde/ambar/rojo) que combina el SoC de
     * bateria y la alarma del congelador (el BLE tiene su propio icono). */
    ui->lbl_health = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_health, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->lbl_health, lv_color_white(), 0);
    lv_obj_set_style_bg_color(ui->lbl_health, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_bg_opa(ui->lbl_health, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(ui->lbl_health, 10, 0);
    lv_obj_set_style_pad_ver(ui->lbl_health, 4, 0);
    lv_obj_set_style_radius(ui->lbl_health, LV_RADIUS_CIRCLE, 0);
    lv_label_set_text(ui->lbl_health, "OK");
    lv_obj_set_size(ui->lbl_health, 60, 38);
    lv_obj_set_style_text_align(ui->lbl_health, LV_TEXT_ALIGN_CENTER, 0);

    /* Indicador AC 220V (PZEM-004T). Aparece solo cuando el modulo PZEM
     * esta presente: si hay tension AC muestra "AC <W>" en verde, si esta
     * conectado pero sin AC muestra "AC" en gris, oculto si no hay PZEM. */
    ui->lbl_ac = lv_label_create(ui->bottom_bar);
    lv_obj_set_style_text_font(ui->lbl_ac, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->lbl_ac, lv_color_hex(0x888888), 0);
    lv_obj_set_style_bg_opa(ui->lbl_ac, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ui->lbl_ac, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(ui->lbl_ac, 4, 0);
    lv_obj_set_style_radius(ui->lbl_ac, 4, 0);
    lv_label_set_text(ui->lbl_ac, "AC");
    lv_obj_set_size(ui->lbl_ac, 110, 38);
    lv_obj_set_style_text_align(ui->lbl_ac, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui->lbl_ac, LV_OBJ_FLAG_HIDDEN);

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
    lv_timer_create(ac_indicator_timer_cb, 2000, ui);
    lv_timer_create(health_timer_cb, 5000, ui);

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
    } else if (d->type == VICTRON_BLE_RECORD_LYNX_SMART_BMS) {
        const victron_record_lynx_smart_bms_t *b = &d->record.lynx;
        trip_computer_on_battery((int32_t)b->battery_current_deci * 100,
                                 b->battery_voltage_centi);
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
                        audio_play_jingle(AUDIO_JINGLE_CRITICAL);
                    }
                    /* Recuperacion */
                    if (soc_pct >= crit_th + 2) s_crit_active = false;
                    /* Cruce a la baja del warning (solo si no ya en critico) */
                    if (s_last_soc >= warn_th && soc_pct < warn_th && soc_pct >= crit_th && !s_warn_active) {
                        s_warn_active = true;
                        audio_play_jingle(AUDIO_JINGLE_WARNING);
                    }
                    if (soc_pct >= warn_th + 2) s_warn_active = false;
                }
                s_last_soc = soc_pct;
            }
            break;
        }
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:
            battery_history_update_latest(BH_SRC_SOLAR_CHARGER,
                (int32_t)d->record.solar.battery_current_deci * 100, 0);
            break;
        case VICTRON_BLE_RECORD_ORION_XS:
            /* Output current = into battery side */
            battery_history_update_latest(BH_SRC_ORION_XS,
                (int32_t)d->record.orion.output_current_deci * 100, 0);
            break;
        case VICTRON_BLE_RECORD_AC_CHARGER:
            battery_history_update_latest(BH_SRC_AC_CHARGER,
                (int32_t)d->record.ac_charger.battery_current_1_deci * 100, 0);
            break;
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
}

void ui_notify_user_activity(void)
{
    ui_state_t *ui = &g_ui;
    if (s_idle_to_live_timer) lv_timer_reset(s_idle_to_live_timer);
    ui_settings_panel_on_user_activity(ui);
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
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    bool new_muted = !audio_is_muted();
    audio_set_mute(new_muted);
    /* Sincronizar el switch del panel Settings si ya está creado */
    if (ui && ui->sound_mute_switch) {
        if (new_muted) lv_obj_add_state(ui->sound_mute_switch, LV_STATE_CHECKED);
        else           lv_obj_clear_state(ui->sound_mute_switch, LV_STATE_CHECKED);
    }
}

/* Toggle Wi-Fi AP on/off al pulsar el icono — comportamiento idéntico al
 * switch del panel Settings: guarda NVS, sincroniza el switch y muestra el
 * mismo modal "Cambio en Wi-Fi requiere reiniciar" con Cancelar / Reiniciar. */
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

    /* Sincronizar el switch del Settings antes de mostrar el modal — si el
     * usuario cancela, el modal volverá a invertir y todo queda coherente. */
    if (ui->wifi.ap_enable) {
        if (en) lv_obj_add_state(ui->wifi.ap_enable, LV_STATE_CHECKED);
        else    lv_obj_clear_state(ui->wifi.ap_enable, LV_STATE_CHECKED);
    }

    ui_show_wifi_restart_dialog(ui);
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

/* Buffer estatico para parsear CSV de un dia. 1440 muestras = 1 min cada una. */
#define FRIGO_LOG_MAX_ENTRIES   1500
static frigo_log_entry_t s_frigo_buf[FRIGO_LOG_MAX_ENTRIES];

static void frigo_chart_load_day(void);
static void frigo_chart_gesture_cb(lv_event_t *e);
static void frigo_chart_touch_cb(lv_event_t *e);
static void frigo_apply_window(void);
static void frigo_update_zoom_label(void);

static void chart_screen_close_cb(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    lv_obj_del(screen);
    s_chart_screen = NULL;
    s_chart = NULL;
}



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

    /* Flechas a izq/dcha del label de fecha para indicar swipe */
    lv_obj_t *lbl_arr_l = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_l, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_l, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_l, LV_SYMBOL_LEFT);
    lv_obj_align(lbl_arr_l, LV_ALIGN_TOP_MID, -120, 14);
    lv_obj_t *lbl_arr_r = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_arr_r, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lbl_arr_r, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(lbl_arr_r, LV_SYMBOL_RIGHT);
    lv_obj_align(lbl_arr_r, LV_ALIGN_TOP_MID, 120, 14);

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
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_bg_color(dot, colores[i], 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_set_pos(dot, 10 + i * 120, 55);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(lbl, colores[i], 0);
        lv_label_set_text(lbl, leyenda[i]);
        lv_obj_set_pos(lbl, 30 + i * 120, 50);
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
    lv_obj_set_size(s_chart, LV_HOR_RES - 150, LV_VER_RES - 140);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart, frigo_chart_touch_cb, LV_EVENT_ALL, NULL);
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
    s_frigo_day_idx = -1;
    frigo_chart_load_day();

    /* Gestures para navegar entre dias */
    lv_obj_add_event_cb(scr, frigo_chart_gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void update_frigo_xlabels_today(int n)
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

static void frigo_chart_load_day(void)
{
    if (!s_chart) return;
    /* Resetear todas las series e indice circular */
    lv_chart_set_all_value(s_chart, s_ser_aletas,     LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_congelador, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_exterior,   LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_ser_fan,        LV_CHART_POINT_NONE);

    if (s_frigo_day_idx < 0) {
        int count = datalogger_get_count();
        if (count < 2) count = 2;
        int wa = (int)(s_frigo_win_a * count);
        int wb = (int)(s_frigo_win_b * count);
        if (wb <= wa) wb = wa + 1;
        if (wb > count) wb = count;
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
        }
        frigo_apply_temp_range(t_min, t_max);
        /* Pasamos `count` (el raw del datalogger), no `valid`: las
         * funciones de xlabels indexan en datalogger_get_entry(idx) que
         * usa el espacio global de indices; pasar `valid` desincroniza
         * los timestamps del rango visible (zoom al 50-100% mostraba
         * etiquetas del 0-50%). El "--:--" ocasional con datalogger
         * vacio es un bug menor que las etiquetas con la hora incorrecta. */
        update_frigo_xlabels_today(count);
        if (s_frigo_lbl_date) lv_label_set_text(s_frigo_lbl_date, "HOY");
        (void)valid;
    } else {
        const char *date = s_frigo_dates[s_frigo_day_idx];
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/frigo/%s.csv", date);
        int n = log_browser_load_frigo(path, s_frigo_buf, FRIGO_LOG_MAX_ENTRIES);
        int wa = (int)(s_frigo_win_a * n);
        int wb = (int)(s_frigo_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > n) wb = n;
        int wn = wb - wa;
        int pts = wn > 0 ? wn : 2;
        lv_chart_set_point_count(s_chart, pts);
        float t_min = 9999.0f, t_max = -9999.0f;
        for (int i = wa; i < wb; i++) {
            const frigo_log_entry_t *e = &s_frigo_buf[i];
            int idx = i - wa;
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
        }
        frigo_apply_temp_range(t_min, t_max);
        update_frigo_xlabels_from_buf(n);
        if (s_frigo_lbl_date) lv_label_set_text(s_frigo_lbl_date, date);
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

static void frigo_chart_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    /* Zoomeado: el pan se hace arrastrando (frigo_chart_touch_cb); ignoramos
     * el swipe para no cambiar de dia sin querer. */
    bool zoomed = (s_frigo_win_a > 0.0001f) || (s_frigo_win_b < 0.9999f);
    if (zoomed) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) {
        if (s_frigo_day_idx == -1) {
            if (s_frigo_n_dates > 0) s_frigo_day_idx = s_frigo_n_dates - 1;
            else return;
        } else if (s_frigo_day_idx > 0) {
            s_frigo_day_idx--;
        } else {
            return;
        }
        s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
        frigo_update_zoom_label();
        frigo_chart_load_day();
    } else if (dir == LV_DIR_LEFT) {
        if (s_frigo_day_idx < 0) return;
        if (s_frigo_day_idx < s_frigo_n_dates - 1) s_frigo_day_idx++;
        else                                       s_frigo_day_idx = -1;
        s_frigo_win_a = 0.0f; s_frigo_win_b = 1.0f;
        frigo_update_zoom_label();
        frigo_chart_load_day();
    }
}

/* --- Pantalla historico bateria --- */
static lv_obj_t *s_bh_screen = NULL;
static lv_obj_t *s_bh_lbl_date = NULL;
static lv_obj_t *s_bh_xlabels  = NULL;
static lv_obj_t *s_bh_lbl_zoom = NULL;
static lv_obj_t *s_bh_totals[BH_SRC_COUNT] = {NULL};
static lv_chart_series_t *s_bh_series[BH_SRC_COUNT] = {NULL};
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
/* En PSRAM: 8800 x 16 B ~= 140 KB, demasiado para RAM interna estatica.
 * Se aloja una vez (lazy) al consultar un dia historico. */
static battery_log_entry_t *s_bh_buf = NULL;

static void bh_chart_load_day(void);
static void bh_chart_gesture_cb(lv_event_t *e);
static void bh_chart_touch_cb(lv_event_t *e);
static void bh_apply_window(void);
static void bh_update_zoom_label(void);

/* Cerrar overlays para rotacion del salvapantallas */
void ui_close_chart_screen(void)
{
    /* Borrar el overlay raíz, que arrastra al chart y todos sus hijos */
    if (s_chart_screen) { lv_obj_del(s_chart_screen); s_chart_screen = NULL; }
    s_chart = NULL;
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
    /* Liberar el buffer de carga del historico (~140KB PSRAM): se reservaba lazy
     * al abrir y nunca se liberaba. Se re-reserva al volver a abrir. */
    if (s_bh_buf) { free(s_bh_buf); s_bh_buf = NULL; }
}

static void bh_screen_close_cb(lv_event_t *e)
{
    (void)e;
    ui_close_battery_history_screen();
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

    lv_obj_t *bh_arr_l = lv_label_create(scr);
    lv_obj_set_style_text_font(bh_arr_l, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bh_arr_l, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(bh_arr_l, LV_SYMBOL_LEFT);
    lv_obj_align(bh_arr_l, LV_ALIGN_TOP_MID, -150, 18);
    lv_obj_t *bh_arr_r = lv_label_create(scr);
    lv_obj_set_style_text_font(bh_arr_r, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bh_arr_r, lv_color_hex(0x8A93A6), 0);
    lv_label_set_text(bh_arr_r, LV_SYMBOL_RIGHT);
    lv_obj_align(bh_arr_r, LV_ALIGN_TOP_MID, 150, 18);

    /* Totales: una sola fila con las 4 fuentes (nombres cortos para que
     * quepan en horizontal sin envolver). */
    lv_obj_t *totals_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(totals_cont);
    lv_obj_set_size(totals_cont, LV_HOR_RES - 32, 26);
    lv_obj_align(totals_cont, LV_ALIGN_TOP_LEFT, 16, 52);
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
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14_es, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(l, "%s +0.0/-0.0 Ah", s_bh_short_names[i]);
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
    lv_obj_set_size(chart, LV_HOR_RES - 150, LV_VER_RES - 178);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(chart, bh_chart_touch_cb, LV_EVENT_ALL, NULL);
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
    s_bh_day_idx = -1;
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

static void bh_update_xlabels_today(int32_t old_ts, int32_t new_ts)
{
    if (!s_bh_xlabels) return;
    int32_t full_span = new_ts - old_ts;
    if (full_span < 0) full_span = 0;
    int32_t t_a = old_ts + (int32_t)((int64_t)full_span * (int64_t)(s_bh_win_a * 1000.0f) / 1000);
    int32_t t_b = old_ts + (int32_t)((int64_t)full_span * (int64_t)(s_bh_win_b * 1000.0f) / 1000);
    int32_t span = t_b - t_a;
    bool have_real_time = (new_ts > 1704067200);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_get_child(s_bh_xlabels, i);
        if (!l) continue;
        if (span <= 0) { lv_label_set_text(l, "--:--"); continue; }
        int32_t ts_at = t_a + (int32_t)((int64_t)span * i / 4);
        if (have_real_time) {
            time_t t = ts_at;
            struct tm tm_local;
            localtime_r(&t, &tm_local);
            lv_label_set_text_fmt(l, "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        } else {
            int32_t age_min = (new_ts - ts_at) / 60;
            lv_label_set_text_fmt(l, "-%dm", (int)age_min);
        }
    }
}

static void bh_update_xlabels_from_buf(int n)
{
    if (!s_bh_xlabels) return;
    if (n <= 0) {
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
                              s_bh_buf[idx].hh, s_bh_buf[idx].mm);
    }
}

static void bh_chart_load_day(void)
{
    if (!s_bh_chart) return;
    bh_clear_chart_series();

    if (s_bh_day_idx < 0) {
        /* HOY: usa el ring buffer en RAM (4 fuentes). Aplica ventana [a, b). */
        const int CHART_MAX_PTS = 300;  /* ver nota en show_battery_history_screen */

        /* Buffer estatico en PSRAM para no fragmentar al cambiar de dia. */
        static bh_point_t *pts = NULL;
        if (pts == NULL) {
            pts = heap_caps_malloc(sizeof(bh_point_t) * BH_POINTS,
                                   MALLOC_CAP_SPIRAM);
        }

        /* Numero REAL de muestras en el ring (no la capacidad BH_POINTS).
         * Todas las fuentes avanzan a la vez en sample_timer_cb, asi que el
         * conteo del BatteryMonitor sirve para las cuatro. El ancho/paso del
         * chart DEBE calcularse sobre estas muestras reales: si se usa
         * BH_POINTS (24h) cuando el equipo lleva pocas horas encendido, la
         * serie se comprime a la izquierda y la franja reciente queda vacia. */
        int n_avail = 0;
        if (pts) {
            int32_t t0 = 0, t1 = 0;
            n_avail = (int)battery_history_get_series(BH_SRC_BATTERY_MONITOR,
                                                      pts, &t0, &t1);
        }
        int win_a_i = (int)(s_bh_win_a * n_avail);
        int win_b_i = (int)(s_bh_win_b * n_avail);
        if (win_b_i <= win_a_i) win_b_i = win_a_i + 1;
        if (win_b_i > n_avail) win_b_i = n_avail;
        int win_count = win_b_i - win_a_i;
        /* Guardia defensiva para no dividir por 0 en chart_step (ring vacio). */
        if (win_count < 2) win_count = 2;
        int chart_pts = (win_count > CHART_MAX_PTS) ? CHART_MAX_PTS : win_count;
        int chart_step = (win_count + chart_pts - 1) / chart_pts;
        if (chart_step < 1) chart_step = 1;
        chart_pts = (win_count + chart_step - 1) / chart_step;
        if (chart_pts < 2) chart_pts = 2;
        lv_chart_set_point_count(s_bh_chart, chart_pts);

        int32_t bmin = INT32_MAX, bmax = INT32_MIN;
        int32_t old_ts_g = INT32_MAX, new_ts_g = INT32_MIN;
        if (pts) {
            for (int s = 0; s < BH_SRC_COUNT; ++s) {
                int32_t ots = 0, nts = 0;
                size_t n = battery_history_get_series((bh_source_t)s, pts, &ots, &nts);
                if (ots > 0 && ots < old_ts_g) old_ts_g = ots;
                if (nts > new_ts_g) new_ts_g = nts;
                /* Ventana per-source: fraccion sobre los puntos realmente
                 * disponibles, no sobre BH_POINTS. Si usaramos win_*_i
                 * (que escalan con la capacidad) y el ring esta solo medio
                 * lleno, zooms cerca del extremo se colapsan a 1-2 puntos.
                 * Reescalamos con el n real de la fuente. */
                int wa, wb;
                if (n == 0) {
                    wa = 0; wb = 0;
                } else {
                    wa = (int)(s_bh_win_a * (float)n);
                    wb = (int)(s_bh_win_b * (float)n);
                    if (wb <= wa) wb = wa + 1;
                    if (wb > (int)n) wb = (int)n;
                    if (wa >= (int)n) wa = (int)n - 1;
                }
                /* En modo tension solo se grafica el BatteryMonitor; las
                 * demas series quedan vacias (ya limpiadas). */
                if (!s_bh_show_voltage || s == BH_SRC_BATTERY_MONITOR) {
                    int idx = 0;
                    for (int k = wa; k < wb && idx < chart_pts; k += chart_step) {
                        int64_t sum = 0; int cnt = 0;
                        int end = (k + chart_step < wb) ? k + chart_step : wb;
                        for (int j = k; j < end; j++) {
                            if (!pts[j].valid) continue;
                            if (s_bh_show_voltage) {
                                if (pts[j].centi_volts > 0) { sum += pts[j].centi_volts; cnt++; }
                            } else {
                                sum += pts[j].milli_amps; cnt++;
                            }
                        }
                        if (cnt > 0) {
                            /* deci-V (centi/10) en tension, deci-A (milli/100) en corriente */
                            int32_t a = s_bh_show_voltage
                                ? (int32_t)((sum / cnt) / 10)
                                : (int32_t)((sum / cnt) / 100);
                            if (a < bmin) bmin = a;
                            if (a > bmax) bmax = a;
                            lv_chart_set_value_by_id(s_bh_chart, s_bh_series[s], idx, (lv_coord_t)a);
                        } else {
                            lv_chart_set_value_by_id(s_bh_chart, s_bh_series[s], idx, LV_CHART_POINT_NONE);
                        }
                        idx++;
                    }
                }
                /* Totales: siempre del dia completo (no de la ventana visible) */
                float ch = 0, dis = 0;
                battery_history_get_totals((bh_source_t)s, &ch, &dis);
                if (s_bh_totals[s]) {
                    lv_label_set_text_fmt(s_bh_totals[s],
                        "%s +%.1f/-%.1f Ah",
                        s_bh_short_names[s], ch, dis);
                }
                /* Yield al scheduler tras cada serie para que IDLE0 corra y
                 * el task_wdt no salte. Con 4 series x ~1500 puntos y un
                 * chart ancho, sin esto monopolizamos taskLVGL > 5 s. */
                vTaskDelay(1);
            }
            /* pts es estatico: no liberar */
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
        if (old_ts_g == INT32_MAX) old_ts_g = 0;
        if (new_ts_g == INT32_MIN) new_ts_g = 0;
        bh_update_xlabels_today(old_ts_g, new_ts_g);
        if (s_bh_lbl_date) lv_label_set_text(s_bh_lbl_date, "HOY (24H)");
    } else {
        /* Día histórico desde SD: solo se carga BM */
        const char *date = s_bh_dates[s_bh_day_idx];
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/bateria/%s.csv", date);
        if (s_bh_buf == NULL) {
            s_bh_buf = heap_caps_malloc(sizeof(battery_log_entry_t) * BH_LOG_MAX_ENTRIES,
                                        MALLOC_CAP_SPIRAM);
        }
        int n = s_bh_buf ? log_browser_load_battery(path, s_bh_buf, BH_LOG_MAX_ENTRIES) : 0;

        /* Totales calculados sobre el dia completo (no afectados por la ventana). */
        int64_t total_ch_ma_s = 0, total_dis_ma_s = 0;
        for (int i = 0; i < n; i++) {
            int32_t ma = s_bh_buf[i].milli_amps;
            if (ma > 0) total_ch_ma_s += ma;
            else        total_dis_ma_s += -ma;
        }

        /* Ventana */
        int wa = (int)(s_bh_win_a * n);
        int wb = (int)(s_bh_win_b * n);
        if (wb <= wa) wb = wa + 1;
        if (wb > n) wb = n;
        int wn = wb - wa;
        int pts_cnt = wn > 0 ? wn : 2;
        if (pts_cnt > 1500) pts_cnt = 1500;
        if (pts_cnt < 2) pts_cnt = 2;
        lv_chart_set_point_count(s_bh_chart, pts_cnt);
        int step = (wn > 1500) ? (wn + 1499) / 1500 : 1;
        int32_t bmin = INT32_MAX, bmax = INT32_MIN;
        int idx = 0;
        for (int i = wa; i < wb; i += step) {
            int32_t a;
            if (s_bh_show_voltage) {
                int32_t cv = s_bh_buf[i].centi_volts;
                if (cv <= 0) {   /* CSV viejo sin columna de tension */
                    lv_chart_set_value_by_id(s_bh_chart, s_bh_series[0], idx,
                                             LV_CHART_POINT_NONE);
                    idx++;
                    continue;
                }
                a = cv / 10;                     /* centi-V -> deci-V */
            } else {
                a = s_bh_buf[i].milli_amps / 100; /* milli-A -> deci-A */
            }
            if (a < bmin) bmin = a;
            if (a > bmax) bmax = a;
            lv_chart_set_value_by_id(s_bh_chart, s_bh_series[0], idx, (lv_coord_t)a);
            idx++;
        }
        /* Las otras 3 series quedan en LV_CHART_POINT_NONE */
        if (bmin == INT32_MAX) {
            if (s_bh_show_voltage) { bmin = 120; bmax = 140; }  /* 12.0-14.0 V */
            else                   { bmin = -40; bmax = 40; }
        }
        int32_t span = bmax - bmin; if (span < 1) span = 1;
        s_bh_y_min = bmin - span / 20 - 1;
        s_bh_y_max = bmax + span / 20 + 1;
        lv_chart_set_range(s_bh_chart, LV_CHART_AXIS_PRIMARY_Y,
                           s_bh_y_min, s_bh_y_max);
        bh_update_xlabels_from_buf(n);
        /* Totales: solo BM tiene datos. Asumimos sample medio de 10 s. */
        float ch = (float)(total_ch_ma_s  * 10) / (1000.0f * 3600.0f);
        float ds = (float)(total_dis_ma_s * 10) / (1000.0f * 3600.0f);
        for (int s = 0; s < BH_SRC_COUNT; ++s) {
            if (!s_bh_totals[s]) continue;
            if (s == BH_SRC_BATTERY_MONITOR) {
                lv_label_set_text_fmt(s_bh_totals[s],
                    "%s +%.1f/-%.1f Ah",
                    s_bh_short_names[s], ch, ds);
            } else {
                lv_label_set_text_fmt(s_bh_totals[s],
                    "%s (s/d)",
                    s_bh_short_names[s]);
            }
        }
        if (s_bh_lbl_date) lv_label_set_text(s_bh_lbl_date, date);
    }
    lv_chart_refresh(s_bh_chart);
}

static void bh_chart_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    /* Zoomeado: el pan se hace arrastrando (bh_chart_touch_cb); ignoramos el
     * swipe para no cambiar de dia sin querer. */
    bool zoomed = (s_bh_win_a > 0.0001f) || (s_bh_win_b < 0.9999f);
    if (zoomed) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) {
        if (s_bh_day_idx == -1) {
            if (s_bh_n_dates > 0) s_bh_day_idx = s_bh_n_dates - 1;
            else return;
        } else if (s_bh_day_idx > 0) {
            s_bh_day_idx--;
        } else {
            return;
        }
        s_bh_win_a = 0.0f; s_bh_win_b = 1.0f;
        bh_update_zoom_label();
        bh_chart_load_day();
    } else if (dir == LV_DIR_LEFT) {
        if (s_bh_day_idx < 0) return;
        if (s_bh_day_idx < s_bh_n_dates - 1) s_bh_day_idx++;
        else                                 s_bh_day_idx = -1;
        s_bh_win_a = 0.0f; s_bh_win_b = 1.0f;
        bh_update_zoom_label();
        bh_chart_load_day();
    }
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
 * Tour de capturas: recorre las pantallas principales con los datos reales
 * del momento y guarda cada una como BMP en /sdcard/screenshots/.
 *
 * LVGL no es thread-safe: la navegacion se hace tomando lvgl_port_lock; el
 * lock se suelta durante las esperas para que lleguen datos BLE reales y se
 * dibuje la vista. screenshot_save_bmp toma su propio lock para copiar el
 * framebuffer. Se dispara solo una vez (marcador en la SD) ~60 s tras el boot.
 * ────────────────────────────────────────────────────────────────────── */
#define TOUR_DIR        "/sdcard/screenshots"
#define TOUR_MARKER     TOUR_DIR "/.done_sim20260706h"  /* subir version fuerza re-ejecutar */
#define TOUR_BOOT_DELAY_MS   60000   /* esperar ~60s a que BLE tenga datos reales */
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

static void screenshot_tour_task(void *arg)
{
    ui_state_t *ui = &g_ui;

    /* Disparo unico: si ya existe el marcador, no repetir en cada arranque. */
    vTaskDelay(pdMS_TO_TICKS(TOUR_BOOT_DELAY_MS));
    FILE *mk = fopen(TOUR_MARKER, "r");
    if (mk) { fclose(mk); ESP_LOGI("TOUR", "Marcador presente, no repito"); vTaskDelete(NULL); return; }
    if (mkdir(TOUR_DIR, 0777) != 0) {
        /* Puede existir ya; si no hay SD, los fopen posteriores fallaran. */
    }
    /* Escribir el marcador ANTES de capturar: si algo falla a mitad del tour,
     * en el siguiente arranque se ve el marcador y NO se repite (evita un
     * bucle de reinicios). Si no hay SD, este fopen falla y se reintentara. */
    { FILE *m = fopen(TOUR_MARKER, "w"); if (m) { fputs("done\n", m); fclose(m); } }

    ESP_LOGI("TOUR", "Iniciando recorrido de capturas...");
    const ui_view_mode_t saved_mode = ui->view_selection.mode;

    static const struct { ui_view_mode_t mode; const char *name; } LIVE_SCREENS[] = {
        { UI_VIEW_MODE_OVERVIEW,        "01_overview"        },
        { UI_VIEW_MODE_DEFAULT_BATTERY, "02_bateria"         },
        { UI_VIEW_MODE_SOLAR_CHARGER,   "03_solar"           },
        { UI_VIEW_MODE_BATTERY_MONITOR, "04_monitor_bateria" },
        { UI_VIEW_MODE_INVERTER,        "05_inversor"        },
        { UI_VIEW_MODE_DCDC_CONVERTER,  "06_dcdc"            },
    };

    int ok = 0;
    char path[96];
    for (size_t i = 0; i < sizeof(LIVE_SCREENS) / sizeof(LIVE_SCREENS[0]); ++i) {
        tour_set_view(ui, LIVE_SCREENS[i].mode);
        tour_settle();
        snprintf(path, sizeof(path), TOUR_DIR "/%s.bmp", LIVE_SCREENS[i].name);
        if (screenshot_save_bmp(path) == ESP_OK) ok++;
    }

    /* Log historico de bateria (overlay) */
    if (lvgl_port_lock(1000)) { ui_show_battery_history_screen(ui); lvgl_port_unlock(); }
    tour_settle();
    if (screenshot_save_bmp(TOUR_DIR "/07_log_bateria.bmp") == ESP_OK) ok++;
    if (lvgl_port_lock(1000)) { ui_close_battery_history_screen(); lvgl_port_unlock(); }

    /* Log de temperaturas del frigo (overlay) */
    if (lvgl_port_lock(1000)) { ui_show_chart_screen(ui); lvgl_port_unlock(); }
    tour_settle();
    if (screenshot_save_bmp(TOUR_DIR "/08_log_frigo.bmp") == ESP_OK) ok++;
    if (lvgl_port_lock(1000)) { ui_close_chart_screen(); lvgl_port_unlock(); }

    /* Menu de Ajustes (pagina principal) */
    if (lvgl_port_lock(1000)) {
        lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
        lvgl_port_unlock();
    }
    tour_settle();
    if (screenshot_save_bmp(TOUR_DIR "/09_ajustes.bmp") == ESP_OK) ok++;

    /* Sub-paginas de Ajustes (Frigo, Logs, Wi-Fi, Display, Sonido, Victron
     * Keys, About). Orden fijado por settings_panel. */
    static const char *SET_NAMES[] = {
        "frigo", "logs", "wifi", "display", "sonido", "victron_keys", "about"
    };
    int n_set = ui_settings_panel_page_count();
    for (int s = 0; s < n_set; ++s) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
            ui_settings_panel_show_page(s);
            lvgl_port_unlock();
        }
        tour_settle();
        const char *nm = (s < (int)(sizeof(SET_NAMES) / sizeof(SET_NAMES[0])))
                             ? SET_NAMES[s] : "pagina";
        snprintf(path, sizeof(path), TOUR_DIR "/1%d_ajustes_%s.bmp", s, nm);
        if (screenshot_save_bmp(path) == ESP_OK) ok++;
    }
    if (lvgl_port_lock(1000)) { ui_settings_panel_go_to_main(); lvgl_port_unlock(); }

    /* Restaurar: volver a Live + Overview con el modo previo. */
    if (lvgl_port_lock(1000)) {
        ui->view_selection.mode = saved_mode;
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }

    ESP_LOGI("TOUR", "Recorrido terminado: %d capturas en %s", ok, TOUR_DIR);
    vTaskDelete(NULL);
}

void ui_start_screenshot_tour(void)
{
    xTaskCreate(screenshot_tour_task, "shot_tour", 12288, NULL, 3, NULL);
}

/* ─── Carrusel de captura a demanda (switch en Settings→Display) ──────────────
 * Recorre SOLO las 8 pantallas de datos (6 device-views + grafico bateria +
 * grafico frigo), captura cada una a BMP en la SD (sobrescribe mismos nombres),
 * y al terminar apaga el switch y muestra el resultado. Reutiliza
 * tour_set_view/tour_settle/screenshot_save_bmp. A diferencia de
 * screenshot_tour_task: sin marcador, sin retardo de 60 s y sin las paginas de
 * Ajustes. Un unico disparo a la vez (s_capture_running). */
static volatile bool s_capture_running = false;

static const struct { ui_view_mode_t mode; const char *name; } CAPTURE_SCREENS[] = {
    { UI_VIEW_MODE_OVERVIEW,        "01_overview"        },
    { UI_VIEW_MODE_DEFAULT_BATTERY, "02_bateria"         },
    { UI_VIEW_MODE_SOLAR_CHARGER,   "03_solar"           },
    { UI_VIEW_MODE_BATTERY_MONITOR, "04_monitor_bateria" },
    { UI_VIEW_MODE_INVERTER,        "05_inversor"        },
    { UI_VIEW_MODE_DCDC_CONVERTER,  "06_dcdc"            },
};

/* Apaga el switch y refleja el resultado (bajo lock LVGL; valida los objetos
 * por si la pagina Display se hubiera reconstruido durante la captura). */
static void capture_carousel_finish(ui_state_t *ui, int ok)
{
    if (lvgl_port_lock(1000)) {
        if (ui->capture_switch && lv_obj_is_valid(ui->capture_switch)) {
            lv_obj_clear_state(ui->capture_switch, LV_STATE_CHECKED);
        }
        if (ui->capture_status_lbl && lv_obj_is_valid(ui->capture_status_lbl)) {
            lv_label_set_text_fmt(ui->capture_status_lbl,
                                  "%d/8 capturas guardadas en la SD", ok);
        }
        lvgl_port_unlock();
    }
}

static void capture_carousel_task(void *arg)
{
    ui_state_t *ui = &g_ui;
    const ui_view_mode_t saved_mode = ui->view_selection.mode;
    int ok = 0;
    char path[96];

    if (mkdir(TOUR_DIR, 0777) != 0) {
        /* Puede existir ya; si no hay SD, los screenshot_save_bmp fallaran. */
    }
    ESP_LOGI("CAPCAR", "Carrusel de captura: 8 pantallas -> %s", TOUR_DIR);

    /* 6 device-views con datos reales */
    for (size_t i = 0; i < sizeof(CAPTURE_SCREENS) / sizeof(CAPTURE_SCREENS[0]); ++i) {
        tour_set_view(ui, CAPTURE_SCREENS[i].mode);
        tour_settle();
        snprintf(path, sizeof(path), TOUR_DIR "/%s.bmp", CAPTURE_SCREENS[i].name);
        if (screenshot_save_bmp(path) == ESP_OK) ok++;
    }

    /* Grafico historico de bateria (overlay) */
    if (lvgl_port_lock(1000)) { ui_show_battery_history_screen(ui); lvgl_port_unlock(); }
    tour_settle();
    if (screenshot_save_bmp(TOUR_DIR "/07_log_bateria.bmp") == ESP_OK) ok++;
    if (lvgl_port_lock(1000)) { ui_close_battery_history_screen(); lvgl_port_unlock(); }

    /* Grafico de temperaturas del frigo (overlay) */
    if (lvgl_port_lock(1000)) { ui_show_chart_screen(ui); lvgl_port_unlock(); }
    tour_settle();
    if (screenshot_save_bmp(TOUR_DIR "/08_log_frigo.bmp") == ESP_OK) ok++;
    if (lvgl_port_lock(1000)) { ui_close_chart_screen(); lvgl_port_unlock(); }

    /* Restaurar: volver a Live + la vista previa. */
    if (lvgl_port_lock(1000)) {
        ui->view_selection.mode = saved_mode;
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }

    ESP_LOGI("CAPCAR", "Carrusel terminado: %d/8 capturas", ok);
    capture_carousel_finish(ui, ok);
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
static const char *TOUR_SET_NAMES[] = {
    "frigo", "logs", "wifi", "display", "sonido", "victron_keys", "about"
};
#define TOUR_N_LIVE   ((int)(sizeof(TOUR_LIVE) / sizeof(TOUR_LIVE[0])))
#define TOUR_I_BATLOG  (TOUR_N_LIVE)       /* 6  */
#define TOUR_I_FRIGLOG (TOUR_N_LIVE + 1)   /* 7  */
#define TOUR_I_SETMAIN (TOUR_N_LIVE + 2)   /* 8  */
#define TOUR_I_SETSUB0 (TOUR_N_LIVE + 3)   /* 9  */

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
