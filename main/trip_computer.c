#include "trip_computer.h"

#include <string.h>
#include <stdint.h>
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "TRIP";
static const char *NVS_NS = "trip";

static trip_computer_t s;
static time_t s_last_sample;
static SemaphoreHandle_t s_mtx;

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "reset", s.reset_epoch);
    nvs_set_i64(h, "secs",  s.seconds_running);
    int32_t whc = (int32_t)s.wh_charged;
    int32_t whd = (int32_t)s.wh_discharged;
    int32_t ahc_m = (int32_t)(s.ah_charged    * 1000.0);
    int32_t ahd_m = (int32_t)(s.ah_discharged * 1000.0);
    nvs_set_i32(h, "wh_c", whc);
    nvs_set_i32(h, "wh_d", whd);
    nvs_set_i32(h, "ah_c", ahc_m);  /* en mAh para preservar decimales */
    nvs_set_i32(h, "ah_d", ahd_m);
    nvs_commit(h);
    nvs_close(h);
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int64_t r = 0, secs = 0;
    int32_t whc = 0, whd = 0, ahc_m = 0, ahd_m = 0;
    nvs_get_i64(h, "reset", &r);
    nvs_get_i64(h, "secs",  &secs);
    nvs_get_i32(h, "wh_c", &whc);
    nvs_get_i32(h, "wh_d", &whd);
    nvs_get_i32(h, "ah_c", &ahc_m);
    nvs_get_i32(h, "ah_d", &ahd_m);
    nvs_close(h);
    s.reset_epoch     = r;
    s.seconds_running = secs;
    s.wh_charged      = (double)whc;
    s.wh_discharged   = (double)whd;
    s.ah_charged      = (double)ahc_m / 1000.0;
    s.ah_discharged   = (double)ahd_m / 1000.0;
}

void trip_computer_init(void)
{
    if (s_mtx == NULL) s_mtx = xSemaphoreCreateMutex();
    load_nvs();
    if (s.reset_epoch == 0) {
        time_t now = time(NULL);
        if (now >= 1000000000L) {
            s.reset_epoch = now;
            persist_locked();
        }
    }
    ESP_LOGI(TAG, "Trip: reset=%lld, Wh +%.1f -%.1f, Ah +%.2f -%.2f, secs=%lld",
             (long long)s.reset_epoch, s.wh_charged, s.wh_discharged,
             s.ah_charged, s.ah_discharged, (long long)s.seconds_running);
}

void trip_computer_on_battery(int32_t i_milli, uint16_t v_centi)
{
    if (!s_mtx) trip_computer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    if (now < 1000000000L) {
        xSemaphoreGive(s_mtx);
        return;
    }
    if (s.reset_epoch == 0) {
        s.reset_epoch = now;
    }
    if (s_last_sample != 0) {
        double dt_s = (double)(now - s_last_sample);
        if (dt_s > 0 && dt_s < 600) {  /* descartar huecos */
            double power_w = (double)v_centi * (double)i_milli / 100000.0;
            double energy_wh = power_w * dt_s / 3600.0;
            double current_a = (double)i_milli / 1000.0;
            double charge_ah = current_a * dt_s / 3600.0;
            if (energy_wh > 0) {
                s.wh_charged += energy_wh;
                s.ah_charged += charge_ah;
            } else {
                s.wh_discharged += -energy_wh;
                s.ah_discharged += -charge_ah;
            }
            s.seconds_running += (int64_t)dt_s;
            /* Persistencia cada 5 min para no degradar la flash (NVS) */
            static time_t s_last_save = 0;
            if (now - s_last_save >= 300) {
                persist_locked();
                s_last_save = now;
            }
        }
    }
    s_last_sample = now;
    xSemaphoreGive(s_mtx);
}

void trip_computer_reset(void)
{
    if (!s_mtx) trip_computer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    s.reset_epoch = (now >= 1000000000L) ? now : 0;
    s.wh_charged = 0;
    s.wh_discharged = 0;
    s.ah_charged = 0;
    s.ah_discharged = 0;
    s.seconds_running = 0;
    s_last_sample = 0;
    persist_locked();
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "Trip reset");
}

void trip_computer_get(trip_computer_t *out)
{
    if (!s_mtx || !out) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s;
    xSemaphoreGive(s_mtx);
}
