#include "battery_history.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>

static void bh_flush_to_sd_impl(void);
static const char *TAG = "bathist";
#define NVS_NS "bathist"
#define BH_LOG_DIR "/sdcard/bateria"
#define BH_FLUSH_INTERVAL_MS 60000

typedef struct {
    bh_point_t points[BH_POINTS];
    size_t     write_idx;
    bool       wrapped;
    /* Acumulador del intervalo activo (entre dos sample_timer ticks) */
    int64_t    acc_sum_ma;
    uint32_t   acc_count;
    int32_t    acc_max_ma;
    int32_t    acc_min_ma;
    int32_t    last_milli;
    bool       has_latest;
    int32_t    last_sample_ts;
} bh_buffer_t;

/* Alojado en PSRAM en init() — 552 KB no caben en internal RAM */
static bh_buffer_t *s_bufs = NULL;
static esp_timer_handle_t s_sample_timer = NULL;
static esp_timer_handle_t s_bh_flush_timer = NULL;
static bool s_bh_dir_ok = false;
/* indices para flush */
static size_t s_bh_last_flushed_idx[BH_SRC_COUNT] = {0};
static bool   s_bh_last_flushed_wrapped[BH_SRC_COUNT] = {false};

static const char *s_source_names[BH_SRC_COUNT] = {
    "BatteryMonitor",
    "SolarCharger",
    "OrionXS",
    "ACCharger",
};

const char *battery_history_source_name(bh_source_t src)
{
    if (src >= BH_SRC_COUNT) return "?";
    return s_source_names[src];
}

static int32_t now_seconds(void)
{
    time_t t = time(NULL);
    /* Si time() todavia no esta sincronizado (epoch < 2024-01-01) usar uptime */
    if (t < 1704067200) {
        return (int32_t)(esp_timer_get_time() / 1000000);
    }
    return (int32_t)t;
}

static void buffer_push(bh_buffer_t *b, int32_t ts, int32_t avg, int32_t mx, int32_t mn, bool valid)
{
    b->points[b->write_idx].ts = ts;
    b->points[b->write_idx].milli_amps = avg;
    b->points[b->write_idx].milli_amps_max = mx;
    b->points[b->write_idx].milli_amps_min = mn;
    b->points[b->write_idx].valid = valid;
    b->write_idx = (b->write_idx + 1) % BH_POINTS;
    if (b->write_idx == 0) b->wrapped = true;
}

void battery_history_update_latest(bh_source_t src, int32_t milli_amps)
{
    if (src >= BH_SRC_COUNT) return;
    bh_buffer_t *b = &s_bufs[src];
    /* Acumular en el intervalo activo para calcular avg/max/min */
    if (b->acc_count == 0) {
        b->acc_max_ma = milli_amps;
        b->acc_min_ma = milli_amps;
    } else {
        if (milli_amps > b->acc_max_ma) b->acc_max_ma = milli_amps;
        if (milli_amps < b->acc_min_ma) b->acc_min_ma = milli_amps;
    }
    b->acc_sum_ma += milli_amps;
    b->acc_count++;
    b->last_milli = milli_amps;
    b->has_latest = true;
}

static void sample_timer_cb(void *arg)
{
    int32_t ts = now_seconds();
    for (int i = 0; i < BH_SRC_COUNT; ++i) {
        bh_buffer_t *b = &s_bufs[i];
        if (b->acc_count > 0) {
            int32_t avg = (int32_t)(b->acc_sum_ma / (int64_t)b->acc_count);
            buffer_push(b, ts, avg, b->acc_max_ma, b->acc_min_ma, true);
            b->last_sample_ts = ts;
        } else if (b->has_latest) {
            /* Sin nuevas muestras este intervalo: repite el último valor */
            buffer_push(b, ts, b->last_milli, b->last_milli, b->last_milli, true);
        } else {
            buffer_push(b, ts, 0, 0, 0, false);
        }
        /* Reset acumulador del siguiente intervalo */
        b->acc_sum_ma = 0;
        b->acc_count = 0;
        b->acc_max_ma = 0;
        b->acc_min_ma = 0;
    }
}

/* Persistencia NVS deshabilitada: el buffer es ~552 KB total y NVS no puede
 * con eso cada 15 min. Los datos sobreviven en SD vía bh_flush_to_sd_impl. */


static void bh_get_day_filename(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    if (tm_local.tm_year > 100) {
        snprintf(buf, len, BH_LOG_DIR "/%04d-%02d-%02d.csv",
                 (int)(tm_local.tm_year + 1900) & 0xFFFF,
                 (int)(tm_local.tm_mon + 1) & 0xFF,
                 (int)tm_local.tm_mday & 0xFF);
    } else {
        snprintf(buf, len, BH_LOG_DIR "/boot.csv");
    }
}

void battery_history_flush(void)
{
    bh_flush_to_sd_impl();
}

static void bh_flush_to_sd_impl(void);
static void bh_flush_to_sd_impl(void)
{
    /* Comprobar si /sdcard existe (datalogger lo monta) */
    struct stat st;
    if (stat("/sdcard", &st) != 0) return;

    /* Crear directorio bateria si hace falta */
    if (!s_bh_dir_ok) {
        if (stat(BH_LOG_DIR, &st) == 0) {
            ESP_LOGI(TAG, "Directorio %s ya existe", BH_LOG_DIR);
            s_bh_dir_ok = true;
        } else {
            int r = mkdir(BH_LOG_DIR, 0775);
            if (r == 0) {
                ESP_LOGI(TAG, "Creado directorio %s", BH_LOG_DIR);
                s_bh_dir_ok = true;
            } else {
                ESP_LOGW(TAG, "mkdir %s falló (errno=%d)", BH_LOG_DIR, errno);
                /* No marcamos como ok, reintentamos en el siguiente ciclo */
                return;
            }
        }
    }

    char path[64];
    bh_get_day_filename(path, sizeof path);
    bool need_header = (stat(path, &st) != 0);

    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s failed (errno=%d: %s)", path, errno, strerror(errno));
        return;
    }
    if (need_header) {
        fprintf(f, "timestamp,source,milli_amps,milli_amps_max,milli_amps_min\n");
    }

    int total_written = 0;
    for (int s = 0; s < BH_SRC_COUNT; ++s) {
        bh_buffer_t *b = &s_bufs[s];
        size_t total = b->wrapped ? BH_POINTS : b->write_idx;
        if (total == 0) continue;
        /* Determinar el rango de puntos nuevos desde el ultimo flush */
        size_t since_idx = s_bh_last_flushed_idx[s];
        bool since_wrapped = s_bh_last_flushed_wrapped[s];
        for (size_t i = 0; i < total; ++i) {
            size_t idx = b->wrapped ? (b->write_idx + i) % BH_POINTS : i;
            /* Saltar lo ya escrito antes:
               como simplificacion, si ts <= last_ts del source ignoramos */
            if (!b->points[idx].valid) continue;
            /* Comparar contra el ts del ultimo flush */
            (void)since_idx; (void)since_wrapped;
            /* Para evitar duplicados: solo escribimos puntos cuyo ts > umbral */
            static int32_t last_ts_per_src[BH_SRC_COUNT] = {0};
            if (b->points[idx].ts <= last_ts_per_src[s]) continue;
            time_t pt = b->points[idx].ts;
            struct tm tmp;
            localtime_r(&pt, &tmp);
            char ts_str[32];
            if (tmp.tm_year > 100) {
                snprintf(ts_str, sizeof ts_str, "%04d-%02d-%02d %02d:%02d:%02d",
                         (int)(tmp.tm_year + 1900) & 0xFFFF,
                         (int)(tmp.tm_mon + 1) & 0xFF,
                         (int)tmp.tm_mday & 0xFF,
                         (int)tmp.tm_hour & 0xFF,
                         (int)tmp.tm_min & 0xFF,
                         (int)tmp.tm_sec & 0xFF);
            } else {
                snprintf(ts_str, sizeof ts_str, "BOOT+%ld", (long)pt);
            }
            fprintf(f, "%s,%s,%ld,%ld,%ld\n",
                    ts_str, battery_history_source_name((bh_source_t)s),
                    (long)b->points[idx].milli_amps,
                    (long)b->points[idx].milli_amps_max,
                    (long)b->points[idx].milli_amps_min);
            last_ts_per_src[s] = b->points[idx].ts;
            total_written++;
        }
        s_bh_last_flushed_idx[s] = b->write_idx;
        s_bh_last_flushed_wrapped[s] = b->wrapped;
    }
    fclose(f);
    if (total_written > 0) {
        ESP_LOGI(TAG, "Volcadas %d entradas a %s", total_written, path);
    }
}

static void bh_flush_timer_cb(void *arg)
{
    bh_flush_to_sd_impl();
}

esp_err_t battery_history_init(void)
{
    /* Alocar buffer en PSRAM (552 KB) */
    if (!s_bufs) {
        s_bufs = heap_caps_calloc(BH_SRC_COUNT, sizeof(bh_buffer_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bufs) {
            ESP_LOGE(TAG, "No se pudo alocar %u bytes en PSRAM",
                     (unsigned)(BH_SRC_COUNT * sizeof(bh_buffer_t)));
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_bufs, 0, BH_SRC_COUNT * sizeof(bh_buffer_t));

    const esp_timer_create_args_t sample_args = {
        .callback = sample_timer_cb,
        .name = "bh_sample",
    };
    ESP_ERROR_CHECK(esp_timer_create(&sample_args, &s_sample_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_sample_timer, (uint64_t)BH_SAMPLE_MS * 1000ULL));

    /* Flush a SD cada 60s */
    const esp_timer_create_args_t flush_args = {
        .callback = bh_flush_timer_cb,
        .name = "bh_flush",
    };
    if (esp_timer_create(&flush_args, &s_bh_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_bh_flush_timer, (uint64_t)BH_FLUSH_INTERVAL_MS * 1000ULL);
        ESP_LOGI(TAG, "BH flush timer iniciado (%d ms)", BH_FLUSH_INTERVAL_MS);
    }

    ESP_LOGI(TAG, "battery_history initialised (sample %dms, %d points)",
             BH_SAMPLE_MS, BH_POINTS);
    return ESP_OK;
}

size_t battery_history_get_series(bh_source_t src,
                                  bh_point_t *out_points,
                                  int32_t *out_oldest_ts,
                                  int32_t *out_newest_ts)
{
    if (src >= BH_SRC_COUNT || !out_points) return 0;
    bh_buffer_t *b = &s_bufs[src];
    size_t count = 0;
    int32_t oldest = INT32_MAX, newest = INT32_MIN;

    /* Reorder so the oldest comes first */
    if (b->wrapped) {
        for (size_t i = 0; i < BH_POINTS; ++i) {
            size_t idx = (b->write_idx + i) % BH_POINTS;
            out_points[count] = b->points[idx];
            if (b->points[idx].valid) {
                if (b->points[idx].ts < oldest) oldest = b->points[idx].ts;
                if (b->points[idx].ts > newest) newest = b->points[idx].ts;
            }
            count++;
        }
    } else {
        for (size_t i = 0; i < b->write_idx; ++i) {
            out_points[count] = b->points[i];
            if (b->points[i].valid) {
                if (b->points[i].ts < oldest) oldest = b->points[i].ts;
                if (b->points[i].ts > newest) newest = b->points[i].ts;
            }
            count++;
        }
    }
    if (out_oldest_ts) *out_oldest_ts = (oldest == INT32_MAX ? 0 : oldest);
    if (out_newest_ts) *out_newest_ts = (newest == INT32_MIN ? 0 : newest);
    return count;
}

void battery_history_get_totals(bh_source_t src,
                                float *out_charge_ah,
                                float *out_discharge_ah)
{
    if (src >= BH_SRC_COUNT) {
        if (out_charge_ah) *out_charge_ah = 0;
        if (out_discharge_ah) *out_discharge_ah = 0;
        return;
    }
    bh_buffer_t *b = &s_bufs[src];
    /* Trapezoidal integration: each step BH_SAMPLE_MS apart.
     * Ah = sum(milli_amps * dt_h) / 1000  with dt_h = 3min/60 = 0.05 */
    const float dt_h = (BH_SAMPLE_MS / 1000.0f) / 3600.0f;
    float charge_mah = 0, discharge_mah = 0;
    size_t total = b->wrapped ? BH_POINTS : b->write_idx;
    for (size_t i = 0; i < total; ++i) {
        if (!b->points[i].valid) continue;
        float ah = b->points[i].milli_amps * dt_h / 1000.0f * 1000.0f; /* kept in mAh */
        if (b->points[i].milli_amps > 0) charge_mah += ah;
        else discharge_mah += -ah;
    }
    if (out_charge_ah) *out_charge_ah = charge_mah / 1000.0f;
    if (out_discharge_ah) *out_discharge_ah = discharge_mah / 1000.0f;
}

void battery_history_get_time_range(bh_source_t src, int32_t *out_oldest_ts, int32_t *out_newest_ts)
{
    if (out_oldest_ts) *out_oldest_ts = 0;
    if (out_newest_ts) *out_newest_ts = 0;
    if (src >= BH_SRC_COUNT) return;
    bh_buffer_t *b = &s_bufs[src];
    int32_t oldest = INT32_MAX, newest = INT32_MIN;
    size_t total = b->wrapped ? BH_POINTS : b->write_idx;
    for (size_t i = 0; i < total; ++i) {
        if (!b->points[i].valid) continue;
        if (b->points[i].ts < oldest) oldest = b->points[i].ts;
        if (b->points[i].ts > newest) newest = b->points[i].ts;
    }
    if (out_oldest_ts) *out_oldest_ts = (oldest == INT32_MAX ? 0 : oldest);
    if (out_newest_ts) *out_newest_ts = (newest == INT32_MIN ? 0 : newest);
}
