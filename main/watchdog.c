#include "watchdog.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "datalogger.h"
#include "battery_history.h"
#include "display.h"

static const char *TAG = "WD";
static const char *NVS_NS = "wd";
static const char *KEY_COUNT = "count";

static uint32_t s_reset_count = 0;
static const char *s_reason_str = "Unknown";

/* Configuración del monitor LVGL */
#define WD_MONITOR_PERIOD_MS   3000   /* cadencia de chequeo */
#define WD_LVGL_LOCK_TIMEOUT   200    /* ms */
#define WD_LVGL_FAIL_THRESHOLD 3      /* fallos consecutivos para reset */

static void wd_increment_counter_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t v = 0;
    nvs_get_u32(h, KEY_COUNT, &v);
    v++;
    nvs_set_u32(h, KEY_COUNT, v);
    nvs_commit(h);
    nvs_close(h);
    s_reset_count = v;
}

static uint32_t wd_load_counter_nvs(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, KEY_COUNT, &v);
        nvs_close(h);
    }
    return v;
}

static const char *reason_to_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External pin";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_PANIC:     return "Panic";
        case ESP_RST_INT_WDT:   return "Watchdog (INT)";
        case ESP_RST_TASK_WDT:  return "Watchdog (TASK)";
        case ESP_RST_WDT:       return "Watchdog (otro)";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brown-out";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_UNKNOWN:   /* fall-through */
        default:                return "Unknown";
    }
}

static void wd_monitor_task(void *arg)
{
    int consecutive_fail = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WD_MONITOR_PERIOD_MS));

        if (lvgl_port_lock(WD_LVGL_LOCK_TIMEOUT)) {
            lvgl_port_unlock();
            consecutive_fail = 0;
        } else {
            consecutive_fail++;
            ESP_LOGW(TAG, "LVGL lock timeout (%d/%d)",
                     consecutive_fail, WD_LVGL_FAIL_THRESHOLD);
            if (consecutive_fail >= WD_LVGL_FAIL_THRESHOLD) {
                ESP_LOGE(TAG, "UI congelada — flush SD y reset controlado");
                /* Apaga backlight para evitar parpadeo durante el reset */
                bsp_display_brightness_set(0);
                datalogger_flush();
                battery_history_flush();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }
}

esp_err_t watchdog_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_reason_str = reason_to_str(r);

    /* Si el ultimo reset fue por watchdog/panic (sintomas de cuelgue),
     * incrementamos el contador para diagnostico posterior. */
    bool is_wdt_reset = (r == ESP_RST_TASK_WDT ||
                         r == ESP_RST_INT_WDT ||
                         r == ESP_RST_WDT     ||
                         r == ESP_RST_PANIC);

    s_reset_count = wd_load_counter_nvs();
    if (is_wdt_reset) {
        wd_increment_counter_nvs();
    }
    ESP_LOGI(TAG, "Reset reason: %s; total WDT/panic resets: %lu",
             s_reason_str, (unsigned long)s_reset_count);

    /* Task monitor de salud de LVGL */
    BaseType_t ok = xTaskCreate(wd_monitor_task, "wd_monitor",
                                3072, NULL, 2, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

uint32_t watchdog_get_reset_count(void)
{
    return s_reset_count;
}

const char *watchdog_last_reset_reason(void)
{
    return s_reason_str;
}
