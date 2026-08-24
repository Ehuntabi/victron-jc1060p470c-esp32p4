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
    int32_t km_m;         /* distancia recorrida, en METROS (10.000 km caben de sobra) */
    uint8_t active;       /* 1 = viaje en curso (ver trip_computer_is_active) */
} trip_snap_t;

/* ── Odometro por GPS ──────────────────────────────────────────────
 * Umbrales elegidos para el muestreo de 5 s de main.c. */
#define GPS_MIN_SATS       5        /* con menos, la posicion da bandazos */
#define GPS_SALTO_MIN_M    15.0     /* parado, el receptor deriva unos metros */
#define GPS_SALTO_MAX_M    1000.0   /* en 5 s son 720 km/h: eso no es conducir */
#define GPS_HUECO_MAX_S    30       /* mas silencio que esto = re-anclar, no unir */

static double  s_gps_lat, s_gps_lon;
static time_t  s_gps_t;
static bool    s_gps_ancla;   /* hay un punto anterior con el que comparar */

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
        .km_m     = (int32_t)(s.km * 1000.0),
        .active   = s.active ? 1 : 0,
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
    nvs_set_i32(h, "km_m", snap->km_m);
    nvs_set_u8(h, "active", snap->active);
    esp_err_t err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "trip computer no persistio: %s", esp_err_to_name(err));
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
    /* Clave nueva (24-ago-2026, con el GPS ya montado): en una placa que venga
     * de antes no existe y nvs_get la deja en 0, que es justo lo correcto. */
    int32_t km_m = 0;
    nvs_get_i32(h, "km_m", &km_m);
    /* Migracion: en una placa que venia de antes de existir el flag no hay clave
     * "active". Se asume viaje EN CURSO, que es el caso real de cualquiera que ya
     * estuviera usando el trip computer: asi el aviso de arranque deja de salir
     * desde la primera actualizacion, sin pedir nada. Quien lo tuviera terminado
     * empieza el nuevo desde Ajustes -> Empezar viaje. */
    uint8_t act = 1;
    nvs_get_u8(h, "active", &act);
    nvs_close(h);
    s.active = (act != 0);
    s.reset_epoch     = r;
    s.seconds_running = secs;
    s.wh_charged      = (double)whc;
    s.wh_discharged   = (double)whd;
    s.ah_charged      = (double)ahc_m / 1000.0;
    s.ah_discharged   = (double)ahd_m / 1000.0;
    s.wh_solar        = (double)whs;
    s.ah_solar        = (double)ahs_m / 1000.0;
    s.solar_seconds   = sol_s;
    s.km              = (double)km_m / 1000.0;
}

/* Metros entre dos posiciones. Equirectangular en vez de haversine: en tramos
 * de segundos el error es despreciable y se ahorra la trigonometria pesada. */
static double metros_entre(double lat1, double lon1, double lat2, double lon2)
{
    const double GRADO_M = 111320.0;
    double dlat = (lat2 - lat1) * GRADO_M;
    double dlon = (lon2 - lon1) * GRADO_M * cos(lat1 * M_PI / 180.0);
    return sqrt(dlat * dlat + dlon * dlon);
}

void trip_computer_on_gps(bool fix, uint8_t satelites, double lat, double lon)
{
    if (!s_mtx) trip_computer_init();
    time_t now = time(NULL);

    /* Sin posicion de fiar no hay ancla: al recuperarla se empieza de nuevo en
     * vez de unir por la recta los dos extremos del tunel. */
    if (!fix || satelites < GPS_MIN_SATS) {
        s_gps_ancla = false;
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_gps_ancla && (now - s_gps_t) <= GPS_HUECO_MAX_S) {
        double d = metros_entre(s_gps_lat, s_gps_lon, lat, lon);
        if (d >= GPS_SALTO_MIN_M && d <= GPS_SALTO_MAX_M) {
            s.km += d / 1000.0;
        } else if (d > GPS_SALTO_MAX_M) {
            ESP_LOGW(TAG, "salto de %.0f m descartado (no se cuenta la recta)", d);
        }
        /* Por debajo del umbral no se suma NI se mueve el ancla: si no, estando
         * parado el ruido iria arrastrando el punto de referencia y el viaje
         * crecería metro a metro sin que el vehiculo se moviera. */
        if (d < GPS_SALTO_MIN_M) { xSemaphoreGive(s_mtx); return; }
    }
    s_gps_lat = lat; s_gps_lon = lon; s_gps_t = now; s_gps_ancla = true;
    xSemaphoreGive(s_mtx);
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

void trip_computer_flush(void)
{
    /* Fuerza el guardado en NVS ahora mismo. Normalmente se hace cada 5 min;
     * al finalizar un viaje hay que asegurarlo antes de apagar o sacar la SD. */
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const trip_snap_t snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    write_nvs(&snap);
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
    s.km = 0;
    s_last_sample = 0;
    s_last_solar_sample = 0;
    s_gps_ancla = false;   /* el primer punto del viaje nuevo no arrastra el anterior */
    s.active = true;            /* empezar un viaje lo deja abierto */
    trip_snap_t snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    write_nvs(&snap);
    ESP_LOGI(TAG, "Trip reset (viaje nuevo, activo)");
}

/* Cambia el flag de viaje abierto/cerrado y persiste, sin tocar los contadores. */
static void trip_set_active(bool on)
{
    if (!s_mtx) trip_computer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s.active = on;
    trip_snap_t snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    write_nvs(&snap);
    ESP_LOGI(TAG, "Viaje %s", on ? "en curso" : "terminado");
}

void trip_computer_end(void) { trip_set_active(false); }

bool trip_computer_is_active(void)
{
    if (!s_mtx) trip_computer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool a = s.active;
    xSemaphoreGive(s_mtx);
    return a;
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
