#include "log_cleanup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "log_cleanup";

static const char *DIRS[] = { "/sdcard/frigo", "/sdcard/bateria" };
#define NUM_DIRS (sizeof(DIRS)/sizeof(DIRS[0]))

/* Parsea YYYY-MM-DD.csv y devuelve epoch a las 00:00 de ese dia, o 0 si no parsea */
static time_t parse_csv_date(const char *fname)
{
    int y = 0, mo = 0, d = 0;
    if (sscanf(fname, "%4d-%2d-%2d.csv", &y, &mo, &d) != 3) return 0;
    if (y < 2024 || y > 2100) return 0;
    if (mo < 1 || mo > 12) return 0;
    if (d < 1 || d > 31) return 0;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    return mktime(&tm);
}

/* Procesa un directorio. Si dry_run, solo cuenta los que serian borrados/avisados.
   threshold_delete: borrar si fecha < (now - max_days * 86400)
   threshold_warn: avisar si fecha < (now - (max_days - 1) * 86400) y >= threshold_delete */
static int process_dir(const char *dir, int max_days, bool dry_run, bool count_warn)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "RTC sin fecha valida, abortando limpieza");
        return 0;
    }
    /* Proteccion: hoy y ayer NUNCA se borran aunque max_days sea 1,
     * para evitar carreras con datalogger_flush / bh_flush en curso. */
    int effective_max = (max_days < 2) ? 2 : max_days;
    time_t cutoff_delete = now - (time_t)effective_max * 86400;
    time_t cutoff_warn   = now - (time_t)(effective_max - 1) * 86400;

    DIR *dp = opendir(dir);
    if (!dp) {
        ESP_LOGD(TAG, "%s no abre (probablemente no montado)", dir);
        return 0;
    }
    int hits = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        time_t ftime = parse_csv_date(de->d_name);
        if (ftime == 0) continue;
        char full_path[300];
        snprintf(full_path, sizeof full_path, "%s/%s", dir, de->d_name);
        if (count_warn) {
            /* Avisar: fecha mas antigua que cutoff_warn pero NO la suficiente para cutoff_delete (por si hay solapamiento) */
            if (ftime < cutoff_warn && ftime >= cutoff_delete) {
                hits++;
            }
        } else {
            /* Modo borrar */
            if (ftime < cutoff_delete) {
                if (!dry_run) {
                    if (remove(full_path) == 0) {
                        ESP_LOGI(TAG, "Borrado %s (antiguedad > %d dias)", full_path, max_days);
                        hits++;
                    } else {
                        ESP_LOGW(TAG, "No se pudo borrar %s", full_path);
                    }
                } else {
                    hits++;
                }
            }
        }
    }
    closedir(dp);
    return hits;
}

int log_cleanup_run_now(int max_days_keep)
{
    int total = 0;
    for (size_t i = 0; i < NUM_DIRS; ++i) {
        total += process_dir(DIRS[i], max_days_keep, false, false);
    }
    if (total > 0) ESP_LOGI(TAG, "Borrados %d ficheros antiguos", total);
    return total;
}

int log_cleanup_files_pending_warning(int max_days_keep)
{
    int total = 0;
    for (size_t i = 0; i < NUM_DIRS; ++i) {
        total += process_dir(DIRS[i], max_days_keep, true, true);
    }
    return total;
}

/* Timer diario (24h) que ejecuta limpieza */
static int s_max_days_cached = 60;
static esp_timer_handle_t s_daily_timer = NULL;

static void daily_cleanup_cb(void *arg)
{
    log_cleanup_run_now(s_max_days_cached);
}

/* Primer barrido tras 5s del boot */
static void initial_cleanup_cb(void *arg)
{
    log_cleanup_run_now(s_max_days_cached);
}

void log_cleanup_init(int max_days_keep)
{
    s_max_days_cached = max_days_keep;
    /* Barrido inicial a los 5s */
    esp_timer_handle_t init_t;
    esp_timer_create_args_t a1 = {
        .callback = initial_cleanup_cb,
        .name = "logclean_init"
    };
    if (esp_timer_create(&a1, &init_t) == ESP_OK) {
        esp_timer_start_once(init_t, 5 * 1000000ULL);
    }
    /* Tarea diaria */
    esp_timer_create_args_t a2 = {
        .callback = daily_cleanup_cb,
        .name = "logclean_daily"
    };
    if (esp_timer_create(&a2, &s_daily_timer) == ESP_OK) {
        /* Cada 24h = 86400 * 1000000 us */
        esp_timer_start_periodic(s_daily_timer, 86400ULL * 1000000ULL);
    }
    ESP_LOGI(TAG, "log_cleanup inicializado (max_days=%d)", max_days_keep);
}
