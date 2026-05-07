/* main.c */
#include <stdio.h>
#include <sys/time.h>
#include <inttypes.h>
#include <lvgl.h>
#include "display.h"
#include "esp_bsp.h"
#include "esp_lvgl_port.h"
#include "victron_ble.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "config_server.h"
#include "frigo.h"
#include "battery_history.h"
#include "log_cleanup.h"
#include "rtc_rx8025t.h"
#include "datalogger.h"
#include "ui/frigo_panel.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"

static const char *TAG = "VICTRON_LVGL_APP";
#define logSection(section) ESP_LOGI(TAG, "\n\n***** %s *****\n", section)
#define LVGL_PORT_ROTATION_DEGREE 90
#define REBOOT_INTERVAL_US (24ULL * 60 * 60 * 1000000) /* 24 horas */
#define LOG_INTERVAL_MS    (5 * 60 * 1000)             /* 5 minutos */
#define ALARM_RISE_MINUTES  30      /* minutos subiendo para alarma */
#define ALARM_TEMP_THRESHOLD -2.0f  /* °C — si supera esto y lleva subiendo 30min */

/* ── Reboot timer ────────────────────────────────────────────── */
static void reboot_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Rebooting after 24h uptime...");
    esp_restart();
}

/* ── Estado UI global ────────────────────────────────────────── */
static ui_state_t *s_ui = NULL;

/* ── Callback frigo: actualiza UI + log cada 5 min ──────────── */
static void frigo_update_cb(const frigo_state_t *state)
{
    if (!s_ui) return;

    /* ── Alarma congelador ─────────────────────────────────────── */
    static float   s_temp_prev     = 0.0f;
    static int64_t s_rising_since  = 0;
    static bool    s_alarm_active  = false;

    float T = state->T_Congelador;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (T > -120.0f) {  /* sensor conectado */
        if (T > s_temp_prev && s_temp_prev > -120.0f) {
            /* temperatura subiendo */
            if (s_rising_since == 0) s_rising_since = now_ms;
            int64_t rising_ms = now_ms - s_rising_since;
            bool alarma = (rising_ms >= (int64_t)ALARM_RISE_MINUTES * 60 * 1000)
                          && (T > ALARM_TEMP_THRESHOLD);
            if (alarma != s_alarm_active) {
                s_alarm_active = alarma;
                if (lvgl_port_lock(50)) {
                    ui_set_freezer_alarm(s_ui, alarma);
                    lvgl_port_unlock();
                }
            }
        } else {
            /* temperatura bajando o estable — resetear contador */
            s_rising_since = 0;
            if (s_alarm_active) {
                s_alarm_active = false;
                if (lvgl_port_lock(50)) {
                    ui_set_freezer_alarm(s_ui, false);
                    lvgl_port_unlock();
                }
            }
        }
        s_temp_prev = T;
    }

    /* ── Actualizar UI ─────────────────────────────────────────── */
    if (lvgl_port_lock(50)) {
        ui_frigo_panel_update(s_ui, state);
        lvgl_port_unlock();
    }

    /* ── Log cada 5 minutos ────────────────────────────────────── */
    static int64_t s_last_log = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_log >= LOG_INTERVAL_MS) {
        s_last_log = now;
        datalogger_log(state);
    }
}

/* ── Touch callback para screensaver ─────────────────────────── */
static void touch_activity_cb(lv_indev_drv_t *drv, uint8_t event)
{
    if (event == LV_EVENT_PRESSED) ui_notify_user_activity();
}


#define APP_WDT_TIMEOUT_S    60    /* reiniciar si LVGL no responde en 60s */
#define BLE_TIMEOUT_S       300    /* alerta si no hay datos BLE en 5 min */


/* Llamar desde frigo_update_cb y ui_on_panel_data para alimentar el WDT */

/* ── app_main ────────────────────────────────────────────────── */
void app_main(void)
{
    /* --- Chip info --- */
    logSection("LVGL init start");
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG,
        "This is %s chip, %d cores, features: %s%s%s%s",
        CONFIG_IDF_TARGET, chip_info.cores,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN)   ? "WiFi/"    : "",
        (chip_info.features & CHIP_FEATURE_BT)         ? "BT/"      : "",
        (chip_info.features & CHIP_FEATURE_BLE)        ? "BLE/"     : "",
        (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : ""
    );

    /* --- Flash and heap info --- */
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(TAG,
        "%" PRIu32 "MB flash, min free heap: %" PRIu32 ", free PSRAM: %u",
        flash_size / (1024 * 1024),
        esp_get_minimum_free_heap_size(),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    /* --- NVS --- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* --- Display --- */
    logSection("Display init");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#else
        .rotate = LV_DISP_ROT_NONE,
#endif
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_brightness_set(80);

    /* --- UI --- */
    if (lvgl_port_lock(0)) {
        s_ui = ui_get_state();
        ui_init();
        lvgl_port_unlock();
    }
    bsp_display_brightness_set(80);

    /* --- Touch callback --- */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        indev->driver->feedback_cb = touch_activity_cb;
    }

    /* --- SD + RTC + Frigo --- */
    esp_err_t sd_err = datalogger_init();
    if (sd_err != ESP_OK)
        ESP_LOGW(TAG, "datalogger_init failed: %s", esp_err_to_name(sd_err));

    esp_err_t rtc_err = rtc_init(bsp_i2c_get_handle());
/* Restaurar hora desde NVS si el RTC da año incorrecto */
    struct tm t_rtc;
    if (rtc_get_time(&t_rtc) == ESP_OK && t_rtc.tm_year < 120) {
        nvs_handle_t nh;
        if (nvs_open("rtc_backup", NVS_READONLY, &nh) == ESP_OK) {
            int64_t epoch = 0;
            if (nvs_get_i64(nh, "epoch", &epoch) == ESP_OK && epoch > 1000000000L) {
                struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "Hora restaurada desde NVS backup: %lld", epoch);
            }
            nvs_close(nh);
        }
    } else if (rtc_get_time(&t_rtc) == ESP_OK && t_rtc.tm_year >= 120) {
        /* RTC tiene hora válida — sincronizar el sistema */
        time_t epoch = mktime(&t_rtc);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Hora del RTC: %04d-%02d-%02d %02d:%02d:%02d",
                 t_rtc.tm_year + 1900, t_rtc.tm_mon + 1, t_rtc.tm_mday,
                 t_rtc.tm_hour, t_rtc.tm_min, t_rtc.tm_sec);
    }
    if (rtc_err != ESP_OK)
        ESP_LOGW(TAG, "rtc_init failed: %s", esp_err_to_name(rtc_err));

    esp_err_t frigo_err = frigo_init(frigo_update_cb);
    if (frigo_err != ESP_OK)
        ESP_LOGW(TAG, "frigo_init failed: %s", esp_err_to_name(frigo_err));

    /* --- WiFi + config server --- */
    wifi_ap_init();
    config_server_start();

    /* --- BLE Victron --- */
    battery_history_init();
    log_cleanup_init(60); /* Borrar logs > 60 dias */
    victron_ble_register_callback(ui_on_panel_data);
    victron_ble_init();

    /* --- Timer reboot 24h --- */
    static esp_timer_handle_t reboot_timer;
    const esp_timer_create_args_t reboot_timer_args = {
        .callback        = &reboot_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "24h_reboot",
    };
    esp_timer_create(&reboot_timer_args, &reboot_timer);
    esp_timer_start_periodic(reboot_timer, REBOOT_INTERVAL_US);


    

    logSection("Setup complete");
}
