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
    double yesterday_pv_wh;  /* snapshot ayer (al rollover de medianoche) */
    double yesterday_loads_wh;
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
#define NVS_KEY_YPV  "ypv_mwh"
#define NVS_KEY_YLD  "yld_mwh"

static void save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_DAY,  s.day_of_year);
    nvs_set_i32(h, NVS_KEY_YEAR, s.year);
    nvs_set_i32(h, NVS_KEY_PV,  (int32_t)s.pv_wh);
    nvs_set_i32(h, NVS_KEY_LD,  (int32_t)s.loads_wh);
    nvs_set_i32(h, NVS_KEY_YPV, (int32_t)s.yesterday_pv_wh);
    nvs_set_i32(h, NVS_KEY_YLD, (int32_t)s.yesterday_loads_wh);
    nvs_commit(h);
    nvs_close(h);
}

static void load_nvs(int today_yday, int today_year)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t day = -1, yr = -1, pv = 0, ld = 0, ypv = 0, yld = 0;
    nvs_get_i32(h, NVS_KEY_DAY,  &day);
    nvs_get_i32(h, NVS_KEY_YEAR, &yr);
    nvs_get_i32(h, NVS_KEY_PV,   &pv);
    nvs_get_i32(h, NVS_KEY_LD,   &ld);
    nvs_get_i32(h, NVS_KEY_YPV,  &ypv);
    nvs_get_i32(h, NVS_KEY_YLD,  &yld);
    nvs_close(h);
    /* "Ayer" se carga siempre que exista, sea cual sea el dia */
    s.yesterday_pv_wh    = (double)ypv;
    s.yesterday_loads_wh = (double)yld;
    if (day == today_yday && yr == today_year) {
        s.pv_wh = (double)pv;
        s.loads_wh = (double)ld;
        ESP_LOGI(TAG, "Restaurado del NVS: PV=%.2f Wh, Loads=%.2f Wh (ayer PV=%.2f Loads=%.2f)",
                 s.pv_wh, s.loads_wh, s.yesterday_pv_wh, s.yesterday_loads_wh);
    } else if (day >= 0) {
        /* El dispositivo arranca un dia distinto al ultimo guardado: el
         * dia previo persistido pasa a ser "ayer" para visualizar tendencia. */
        s.yesterday_pv_wh    = (double)pv;
        s.yesterday_loads_wh = (double)ld;
        ESP_LOGI(TAG, "Dia nuevo al boot: snapshot anterior pasa a 'ayer'");
    }
}

static void check_day_rollover_locked(void)
{
    time_t now = time(NULL);
    if (now < 1000000000L) return;  /* hora aun no valida */
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_yday != s.day_of_year || (t.tm_year + 1900) != s.year) {
        ESP_LOGI(TAG, "Cambio de dia detectado: hoy -> ayer (PV=%.2f Loads=%.2f Wh)",
                 s.pv_wh, s.loads_wh);
        /* Snapshot del dia que termina como "ayer" */
        s.yesterday_pv_wh    = s.pv_wh;
        s.yesterday_loads_wh = s.loads_wh;
        s.pv_wh    = 0;
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
            /* Persistencia cada 5 min para no degradar la flash (NVS) */
            static time_t last_save = 0;
            if (now - last_save >= 300) {
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

float energy_yesterday_pv_kwh(void)
{
    if (!s.mtx) return 0;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    float v = (float)s.yesterday_pv_wh / 1000.0f;
    xSemaphoreGive(s.mtx);
    return v;
}

float energy_yesterday_loads_kwh(void)
{
    if (!s.mtx) return 0;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    float v = (float)s.yesterday_loads_wh / 1000.0f;
    xSemaphoreGive(s.mtx);
    return v;
}
