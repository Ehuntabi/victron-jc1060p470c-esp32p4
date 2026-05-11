#include "energy_today.h"

#include <time.h>
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ENERGY";
static const char *NVS_NS = "energy";

static struct {
    double pv_wh;            /* energía cargada hoy (mWh internamente) */
    double loads_wh;         /* energía consumida hoy */
    uint16_t solar_yield_centikwh;  /* último valor reportado por SmartSolar */
    int day_of_year;
    int year;
    time_t last_sample;      /* timestamp del último update de batería */
    SemaphoreHandle_t mtx;
} s = {0};

#define NVS_KEY_DAY  "day"
#define NVS_KEY_YEAR "year"
#define NVS_KEY_PV   "pv_mwh"
#define NVS_KEY_LD   "ld_mwh"

static void save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_DAY,  s.day_of_year);
    nvs_set_i32(h, NVS_KEY_YEAR, s.year);
    int32_t pv_mwh = (int32_t)s.pv_wh;
    int32_t ld_mwh = (int32_t)s.loads_wh;
    nvs_set_i32(h, NVS_KEY_PV, pv_mwh);
    nvs_set_i32(h, NVS_KEY_LD, ld_mwh);
    nvs_commit(h);
    nvs_close(h);
}

static void load_nvs(int today_yday, int today_year)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t day = -1, yr = -1, pv = 0, ld = 0;
    nvs_get_i32(h, NVS_KEY_DAY,  &day);
    nvs_get_i32(h, NVS_KEY_YEAR, &yr);
    nvs_get_i32(h, NVS_KEY_PV,   &pv);
    nvs_get_i32(h, NVS_KEY_LD,   &ld);
    nvs_close(h);
    if (day == today_yday && yr == today_year) {
        s.pv_wh = (double)pv;
        s.loads_wh = (double)ld;
        ESP_LOGI(TAG, "Restaurado del NVS: PV=%.2f Wh, Loads=%.2f Wh",
                 s.pv_wh, s.loads_wh);
    } else {
        ESP_LOGI(TAG, "Dia nuevo: reset acumuladores");
    }
}

static void check_day_rollover_locked(void)
{
    time_t now = time(NULL);
    if (now < 1000000000L) return;  /* hora aun no valida */
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_yday != s.day_of_year || (t.tm_year + 1900) != s.year) {
        ESP_LOGI(TAG, "Cambio de dia detectado, reset");
        s.pv_wh = 0;
        s.loads_wh = 0;
        s.day_of_year = t.tm_yday;
        s.year = t.tm_year + 1900;
        save_nvs();
    }
}

void energy_today_init(void)
{
    if (s.mtx == NULL) s.mtx = xSemaphoreCreateMutex();

    time_t now = time(NULL);
    if (now < 1000000000L) {
        /* sin RTC valido: deja a 0; se inicializa correctamente cuando llegue
         * el primer sample tras tener hora */
        s.day_of_year = -1;
        s.year = -1;
        return;
    }
    struct tm t;
    localtime_r(&now, &t);
    s.day_of_year = t.tm_yday;
    s.year = t.tm_year + 1900;
    load_nvs(t.tm_yday, t.tm_year + 1900);
}

void energy_today_on_battery(int32_t i_milli, uint16_t v_centi)
{
    if (!s.mtx) energy_today_init();
    xSemaphoreTake(s.mtx, portMAX_DELAY);

    time_t now = time(NULL);
    if (now < 1000000000L) {
        xSemaphoreGive(s.mtx);
        return;
    }
    check_day_rollover_locked();

    /* Integramos entre samples reales: necesitamos al menos un sample previo. */
    if (s.last_sample != 0) {
        double dt_s = (double)(now - s.last_sample);
        if (dt_s > 0 && dt_s < 600) {  /* descartar huecos > 10 min */
            /* P_W = V * I (V en 0.01 V, I en 0.001 A) */
            double power_w = (double)v_centi * (double)i_milli / 100000.0;
            double energy_wh = power_w * dt_s / 3600.0;
            if (energy_wh > 0) {
                s.pv_wh += energy_wh;        /* carga */
            } else {
                s.loads_wh += -energy_wh;    /* descarga */
            }
            /* Persiste cada ~60 s aprox */
            static time_t last_save = 0;
            if (now - last_save >= 60) {
                save_nvs();
                last_save = now;
            }
        }
    }
    s.last_sample = now;
    xSemaphoreGive(s.mtx);
}

void energy_today_on_solar_yield(uint16_t yield_centikwh)
{
    if (!s.mtx) energy_today_init();
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.solar_yield_centikwh = yield_centikwh;
    xSemaphoreGive(s.mtx);
}

float energy_today_pv_kwh(void)
{
    if (!s.mtx) return 0;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    /* Si tenemos yield del SmartSolar lo preferimos (mas fiable que integrar
     * con BMV) */
    float v;
    if (s.solar_yield_centikwh > 0) {
        v = (float)s.solar_yield_centikwh / 100.0f;
    } else {
        v = (float)s.pv_wh / 1000.0f;
    }
    xSemaphoreGive(s.mtx);
    return v;
}

float energy_today_loads_kwh(void)
{
    if (!s.mtx) return 0;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    float v = (float)s.loads_wh / 1000.0f;
    xSemaphoreGive(s.mtx);
    return v;
}

bool energy_today_is_fresh(void)
{
    return s.last_sample != 0;
}
