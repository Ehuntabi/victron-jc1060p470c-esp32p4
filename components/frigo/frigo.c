#include "frigo.h"
#include <inttypes.h>
#include "onewire_bus.h"
#include "ds18b20.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FRIGO";

#define NVS_NS           "frigo"
#define NVS_KEY_ASSIGN   "assign"
#define NVS_KEY_TMIN     "tmin"
#define NVS_KEY_TMAX     "tmax"

#define READ_INTERVAL_MS   2000
#define DS18B20_CONV_MS     800
#define LEDC_CHANNEL       LEDC_CHANNEL_0
#define LEDC_TIMER         LEDC_TIMER_0
#define LEDC_RESOLUTION    LEDC_TIMER_10_BIT
#define FAN_HYST_DEG       0.5f

static frigo_state_t      s_state = {
    .T_Aletas     = -127.0f,
    .T_Congelador = -127.0f,
    .T_Exterior   = -127.0f,
    .T_min        = 35,
    .T_max        = 45,
    .assignment   = {0, 1, 2},
};
static SemaphoreHandle_t  s_mutex = NULL;
static frigo_update_cb_t  s_cb    = NULL;
static onewire_bus_handle_t s_bus  = NULL;
static ds18b20_device_handle_t s_devs[FRIGO_MAX_SENSORS] = {0};

/* ── NVS ─────────────────────────────────────────────────────── */
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t buf[FRIGO_MAX_SENSORS];
    size_t  len = sizeof(buf);
    if (nvs_get_blob(h, NVS_KEY_ASSIGN, buf, &len) == ESP_OK)
        memcpy(s_state.assignment, buf, FRIGO_MAX_SENSORS);
    uint8_t v;
    if (nvs_get_u8(h, NVS_KEY_TMIN, &v) == ESP_OK) s_state.T_min = v;
    if (nvs_get_u8(h, NVS_KEY_TMAX, &v) == ESP_OK) s_state.T_max = v;
    nvs_close(h);
}

/* NVS save en tarea dedicada para no bloquear LVGL */
static void nvs_save_task(void *arg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            nvs_set_blob(h, NVS_KEY_ASSIGN, s_state.assignment, FRIGO_MAX_SENSORS);
            nvs_set_u8(h, NVS_KEY_TMIN, s_state.T_min);
            nvs_set_u8(h, NVS_KEY_TMAX, s_state.T_max);
            xSemaphoreGive(s_mutex);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelete(NULL);
}

static void nvs_save(void)
{
    xTaskCreate(nvs_save_task, "frigo_nvs", 3072, NULL, 3, NULL);
}

/* ── PWM ─────────────────────────────────────────────────────── */
static void fan_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = FRIGO_FAN_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = FRIGO_FAN_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
}

static void fan_set_percent(uint8_t pct)
{
    uint32_t duty = ((uint32_t)pct * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

/* ── Lógica ventilador proporcional con histéresis ───────────── */
static uint8_t compute_fan(float t_aletas, uint8_t t_min, uint8_t t_max,
                            uint8_t fan_prev)
{
    if (t_aletas < -120.0f) return 0;

    float tmin = (float)t_min;
    float tmax = (float)t_max;

    if (t_aletas < tmin - FAN_HYST_DEG) return 0;
    if (t_aletas > tmax + FAN_HYST_DEG) return 100;

    if (t_aletas <= tmin) {
        if (fan_prev == 0) return 0;
        return 20;
    }

    float ratio = (t_aletas - tmin) / (tmax - tmin);
    uint8_t pct = (uint8_t)(20.0f + ratio * 80.0f);
    if (pct > 100) pct = 100;
    return pct;
}

/* ── Tarea de lectura ────────────────────────────────────────── */
static void frigo_task(void *arg)
{
    while (1) {

       
        if (s_state.n_sensors == 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));

        float temps[FRIGO_MAX_SENSORS] = {-127.0f, -127.0f, -127.0f};

        /* Broadcast: disparar conversión en todos a la vez */
        for (int i = 0; i < s_state.n_sensors; i++)
            ds18b20_trigger_temperature_conversion(s_devs[i]);
        vTaskDelay(pdMS_TO_TICKS(DS18B20_CONV_MS));

        for (int i = 0; i < s_state.n_sensors; i++) {
            float t;
            if (ds18b20_get_temperature(s_devs[i], &t) == ESP_OK)
                temps[i] = t;
        }

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            #define TEMP_INVALID(t) ((t) < -120.0f || (t) > 125.0f)
            float ta = temps[s_state.assignment[FRIGO_SLOT_ALETAS]];
            float tc = temps[s_state.assignment[FRIGO_SLOT_CONGELADOR]];
            float te = temps[s_state.assignment[FRIGO_SLOT_EXTERIOR]];
            s_state.T_Aletas     = TEMP_INVALID(ta) ? -127.0f : ta;
            s_state.T_Congelador = TEMP_INVALID(tc) ? -127.0f : tc;
            s_state.T_Exterior   = TEMP_INVALID(te) ? -127.0f : te;

            uint8_t pct = compute_fan(s_state.T_Aletas, s_state.T_min,
                                      s_state.T_max, s_state.fan_percent);
            if (pct != s_state.fan_percent) {
                s_state.fan_percent = pct;
                fan_set_percent(pct);
                ESP_LOGI(TAG, "Fan → %d%%  T_Aletas=%.1f T_min=%d T_max=%d",
                         pct, s_state.T_Aletas, s_state.T_min, s_state.T_max);
            }
            xSemaphoreGive(s_mutex);
        }

        if (s_cb) s_cb(&s_state);
    }
    vTaskDelete(NULL);
}

/* ── API pública ─────────────────────────────────────────────── */
esp_err_t frigo_init(frigo_update_cb_t cb)
{
    s_cb = cb;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "No se pudo crear mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load();

    onewire_bus_config_t bus_cfg = { .bus_gpio_num = FRIGO_ONEWIRE_GPIO };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    ESP_RETURN_ON_ERROR(
        onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus),
        TAG, "onewire_new_bus_rmt falló");

    onewire_device_iter_handle_t iter;
    onewire_new_device_iter(s_bus, &iter);
    onewire_device_t dev;
    s_state.n_sensors = 0;

    while (onewire_device_iter_get_next(iter, &dev) == ESP_OK
           && s_state.n_sensors < FRIGO_MAX_SENSORS) {
        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&dev, &ds_cfg,
                &s_devs[s_state.n_sensors]) == ESP_OK) {
            s_state.sensors[s_state.n_sensors].address = dev.address;
            s_state.sensors[s_state.n_sensors].valid   = true;
            ESP_LOGI(TAG, "DS18B20 [%d] encontrado", s_state.n_sensors);
            s_state.n_sensors++;
        }
    }
    onewire_del_device_iter(iter);
    ESP_LOGI(TAG, "%d sensor(es) DS18B20 en bus GPIO%d",
             s_state.n_sensors, FRIGO_ONEWIRE_GPIO);

    for (int i = 0; i < FRIGO_MAX_SENSORS; i++) {
        if (s_state.assignment[i] >= s_state.n_sensors)
            s_state.assignment[i] = 0;
    }

    fan_pwm_init();
    fan_set_percent(0);

    xTaskCreate(frigo_task, "frigo", 8192, NULL, 5, NULL);
    return ESP_OK;
}

const frigo_state_t *frigo_get_state(void) { return &s_state; }

esp_err_t frigo_set_assignment(frigo_slot_t slot, uint8_t sensor_idx)
{
    if (slot >= FRIGO_MAX_SENSORS || sensor_idx >= s_state.n_sensors)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.assignment[slot] = sensor_idx;
        xSemaphoreGive(s_mutex);
    }
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_set_thresholds(uint8_t t_min, uint8_t t_max)
{
    if (t_min < 30 || t_max > 50 || t_min >= t_max) return ESP_ERR_INVALID_ARG;
    if (t_min % 5 != 0 || t_max % 5 != 0)           return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.T_min = t_min;
        s_state.T_max = t_max;
        xSemaphoreGive(s_mutex);
    }
    nvs_save();
    return ESP_OK;
}

void frigo_addr_to_str(const frigo_sensor_addr_t *sensor, char *buf, size_t len)
{
    uint64_t a = sensor->address;
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(a>>56), (uint8_t)(a>>48), (uint8_t)(a>>40), (uint8_t)(a>>32),
             (uint8_t)(a>>24), (uint8_t)(a>>16), (uint8_t)(a>>8),  (uint8_t)(a));
}
