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
static time_t s_last_solar_sample;   /* stream propio de la solar (MPPT) */
static SemaphoreHandle_t s_mtx;

/* Snapshot para persistir fuera del lock (el commit/GC del NVS no debe retener
 * s_mtx: trip_computer_get quedaria bloqueado desde LVGL durante el commit). */
typedef struct {
    int64_t reset_epoch, seconds_running;
    int32_t whc, whd, ahc_m, ahd_m;
    int32_t whs, ahs_m;   /* aporte solar: Wh y mAh */
    int64_t sol_secs;     /* tiempo con la placa cargando */
} trip_snap_t;

/* Copia el estado a un snapshot. El caller debe tener s_mtx tomado. */
static trip_snap_t trip_snapshot_locked(void)
{
    trip_snap_t snap = {
        .reset_epoch     = s.reset_epoch,
        .seconds_running = s.seconds_running,
        .whc   = (int32_t)s.wh_charged,
        .whd   = (int32_t)s.wh_discharged,
        .ahc_m = (int32_t)(s.ah_charged    * 1000.0),  /* en mAh: preserva decimales */
        .ahd_m = (int32_t)(s.ah_discharged * 1000.0),
        .whs   = (int32_t)s.wh_solar,
        .ahs_m = (int32_t)(s.ah_solar      * 1000.0),
        .sol_secs = s.solar_seconds,
    };
    return snap;
}

static void write_nvs(const trip_snap_t *snap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "reset", snap->reset_epoch);
    nvs_set_i64(h, "secs",  snap->seconds_running);
    nvs_set_i32(h, "wh_c", snap->whc);
    nvs_set_i32(h, "wh_d", snap->whd);
    nvs_set_i32(h, "ah_c", snap->ahc_m);
    nvs_set_i32(h, "ah_d", snap->ahd_m);
    nvs_set_i32(h, "wh_s", snap->whs);
    nvs_set_i32(h, "ah_s", snap->ahs_m);
    nvs_set_i64(h, "sol_t", snap->sol_secs);
    nvs_commit(h);
    nvs_close(h);
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int64_t r = 0, secs = 0;
    int32_t whc = 0, whd = 0, ahc_m = 0, ahd_m = 0, whs = 0, ahs_m = 0;
    int64_t sol_s = 0;
    nvs_get_i64(h, "reset", &r);
    nvs_get_i64(h, "secs",  &secs);
    nvs_get_i32(h, "wh_c", &whc);
    nvs_get_i32(h, "wh_d", &whd);
    nvs_get_i32(h, "ah_c", &ahc_m);
    nvs_get_i32(h, "ah_d", &ahd_m);
    nvs_get_i32(h, "wh_s", &whs);
    nvs_get_i32(h, "ah_s", &ahs_m);
    nvs_get_i64(h, "sol_t", &sol_s);
    nvs_close(h);
    s.reset_epoch     = r;
    s.seconds_running = secs;
    s.wh_charged      = (double)whc;
    s.wh_discharged   = (double)whd;
    s.ah_charged      = (double)ahc_m / 1000.0;
    s.ah_discharged   = (double)ahd_m / 1000.0;
    s.wh_solar        = (double)whs;
    s.ah_solar        = (double)ahs_m / 1000.0;
    s.solar_seconds   = sol_s;
}

void trip_computer_init(void)
{
    if (s_mtx == NULL) s_mtx = xSemaphoreCreateMutex();
    load_nvs();
    if (s.reset_epoch == 0) {
        time_t now = time(NULL);
        if (now >= 1000000000L) {
            s.reset_epoch = now;
            trip_snap_t snap = trip_snapshot_locked();
            write_nvs(&snap);
        }
    }
    ESP_LOGI(TAG, "Trip: reset=%lld, Wh +%.1f -%.1f, Ah +%.2f -%.2f, secs=%lld",
             (long long)s.reset_epoch, s.wh_charged, s.wh_discharged,
             s.ah_charged, s.ah_discharged, (long long)s.seconds_running);
}

void trip_computer_on_battery(int32_t i_milli, uint16_t v_centi)
{
    if (!s_mtx) trip_computer_init();
    bool do_save = false;
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
                do_save = true;
                s_last_save = now;
            }
        }
    }
    s_last_sample = now;

    /* Snapshot bajo lock; el commit a flash se hace FUERA del lock para no
     * bloquear a trip_computer_get (LVGL/dashboard) durante el commit/GC. */
    trip_snap_t snap;
    if (do_save) snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    if (do_save) write_nvs(&snap);
}

void trip_computer_on_solar(int32_t i_milli, uint16_t v_centi)
{
    if (!s_mtx) trip_computer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    if (now < 1000000000L) {
        xSemaphoreGive(s_mtx);
        return;
    }
    /* Solo integra aporte real (la MPPT solo carga: corriente >= 0). El guardado
     * a NVS lo hace el hook del BMV con su cadencia de 5 min (el snapshot ya
     * incluye el solar), asi no duplicamos escrituras de flash. */
    if (s_last_solar_sample != 0 && i_milli > 0) {
        double dt_s = (double)(now - s_last_solar_sample);
        if (dt_s > 0 && dt_s < 600) {  /* descartar huecos */
            double power_w = (double)v_centi * (double)i_milli / 100000.0;
            double current_a = (double)i_milli / 1000.0;
            s.wh_solar += power_w   * dt_s / 3600.0;
            s.ah_solar += current_a * dt_s / 3600.0;
            s.solar_seconds += (int64_t)dt_s;   /* tiempo con la placa cargando */
        }
    }
    s_last_solar_sample = now;
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
    s.wh_solar = 0;
    s.ah_solar = 0;
    s.solar_seconds = 0;
    s.seconds_running = 0;
    s_last_sample = 0;
    s_last_solar_sample = 0;
    trip_snap_t snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    write_nvs(&snap);
    ESP_LOGI(TAG, "Trip reset");
}

void trip_computer_get(trip_computer_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));  /* nunca dejar *out sin init (ventana de arranque) */
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s;
    xSemaphoreGive(s_mtx);
}
