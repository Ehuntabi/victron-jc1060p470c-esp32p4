#include "datalogger.h"
#include "rtc_rx8025t.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/stat.h>

static void flush_pending_to_sd_impl(void);
static const char *TAG = "DATALOGGER";

#define MOUNT_POINT "/sdcard"
#define LOG_DIR     MOUNT_POINT "/frigo"
#define FLUSH_INTERVAL_MS 60000  /* volcado cada 60s */

static datalogger_entry_t s_buf[DATALOGGER_MAX_ENTRIES];
static int                s_head  = 0;
static int                s_count = 0;
static SemaphoreHandle_t  s_mutex = NULL;

/* Indice del primer entry pendiente de volcar a SD (en el orden de RAM) */
static int                s_pending_first = 0;
/* Numero de entries pendientes (escritas al buffer pero no a SD aun) */
static int                s_pending_count = 0;

static sdmmc_card_t *s_card = NULL;
static esp_ldo_channel_handle_t s_sd_ldo = NULL;
static bool          s_sd_mounted = false;
static esp_timer_handle_t s_flush_timer = NULL;
/* Serializa los flushes (timer + main + shutdown) para que dos fprintf
 * concurrentes al mismo CSV no interleaven bytes. */
static SemaphoreHandle_t s_flush_mutex = NULL;

static void get_timestamp(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    if (t.tm_year > 100) {
        snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        uint64_t ms = esp_timer_get_time() / 1000;
        uint32_t s  = (uint32_t)(ms / 1000);
        uint32_t h  = s / 3600; s %= 3600;
        uint32_t m  = s / 60;   s %= 60;
        snprintf(buf, len, "BOOT+%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

static void get_day_filename(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    if (t.tm_year > 100) {
        snprintf(buf, len, LOG_DIR "/%04d-%02d-%02d.csv",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    } else {
        snprintf(buf, len, LOG_DIR "/boot.csv");
    }
}

static esp_err_t mount_sd(void)
{
    /* Activar LDO interno ch4 para alimentar la microSD (TF_VCC) */
    if (!s_sd_ldo) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = 4,
            .voltage_mv = 3300,
        };
        esp_err_t ldo_err = esp_ldo_acquire_channel(&ldo_cfg, &s_sd_ldo);
        if (ldo_err != ESP_OK) {
            ESP_LOGW(TAG, "ldo_acquire ch4 failed: %s", esp_err_to_name(ldo_err));
        } else {
            ESP_LOGI(TAG, "TF_VCC LDO ch4 @ 3300 mV ON");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  /* 4-bit bus */
    /* Pines via IOMUX en slot 0 del P4: 43,44,39,40,41,42 */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD montada OK");
    sdmmc_card_print_info(stdout, s_card);
    /* Crear directorio frigo si no existe */
    struct stat st;
    if (stat(LOG_DIR, &st) != 0) {
        mkdir(LOG_DIR, 0775);
        ESP_LOGI(TAG, "Creado directorio %s", LOG_DIR);
    }
    return ESP_OK;
}

static void format_temp(char *buf, size_t len, float t)
{
    if (t < -120.0f) snprintf(buf, len, "---");
    else snprintf(buf, len, "%.1f", t);
}

void datalogger_flush(void)
{
    flush_pending_to_sd_impl();
}

static void flush_pending_to_sd_impl(void);
static void flush_pending_to_sd_impl(void)
{
    if (!s_sd_mounted || !s_mutex) return;
    if (s_pending_count <= 0) return;

    /* Serializa flushes concurrentes (timer + main thread). */
    if (s_flush_mutex && xSemaphoreTake(s_flush_mutex, 0) != pdTRUE) {
        return;
    }

    char path[64];
    get_day_filename(path, sizeof path);

    /* fopen ANTES de tocar el estado pendiente: si falla preservamos las
     * entradas para el proximo intento. */
    struct stat st;
    bool need_header = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s failed", path);
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }

    /* Mantenemos s_mutex durante toda la escritura. La alternativa "2 fases
     * con snapshot" no se puede aplicar aqui porque s_pending_count nunca
     * decrementa (datalogger_log lo satura en MAX) y por tanto un overflow
     * del ring durante un fprintf prolongado provoca perdida silenciosa.
     * datalogger_log se llama cada ~5 min (frigo update); bloquearlo unos
     * cientos de ms en el flush es ~0.1% del tiempo. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        fclose(f);
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }

    bool io_error = false;
    if (need_header) {
        if (fprintf(f, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n") < 0) {
            io_error = true;
        }
    }
    int snapshot_count = s_pending_count;
    int written = 0;
    for (int i = 0; i < snapshot_count && !io_error; ++i) {
        int idx = (s_pending_first + i) % DATALOGGER_MAX_ENTRIES;
        const datalogger_entry_t *e = &s_buf[idx];
        char ta[10], tc[10], te[10];
        format_temp(ta, sizeof ta, e->T_Aletas);
        format_temp(tc, sizeof tc, e->T_Congelador);
        format_temp(te, sizeof te, e->T_Exterior);
        int r = fprintf(f, "%s,%s,%s,%s,%d\n",
                        e->timestamp, ta, tc, te, e->fan_percent);
        if (r < 0 || ferror(f)) { io_error = true; break; }
        written++;
    }

    /* Solo avanzamos el cursor si la escritura fue limpia. */
    if (!io_error) {
        s_pending_first = (s_pending_first + written) % DATALOGGER_MAX_ENTRIES;
        s_pending_count -= written;
    }
    xSemaphoreGive(s_mutex);
    fclose(f);

    if (io_error) {
        ESP_LOGW(TAG, "I/O error en %s tras %d/%d entradas; reintento proximo flush",
                 path, written, snapshot_count);
    } else if (written > 0) {
        ESP_LOGI(TAG, "Volcadas %d entradas a %s", written, path);
    }
    if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
}

static void flush_timer_cb(void *arg)
{
    flush_pending_to_sd_impl();
}

esp_err_t datalogger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_flush_mutex = xSemaphoreCreateMutex();
    if (!s_flush_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_head  = 0;
    s_count = 0;
    s_pending_first = 0;
    s_pending_count = 0;

    /* Intentar montar SD */
    if (mount_sd() == ESP_OK) {
        s_sd_mounted = true;
        const esp_timer_create_args_t args = {
            .callback = flush_timer_cb,
            .name = "dl_flush",
        };
        if (esp_timer_create(&args, &s_flush_timer) == ESP_OK) {
            esp_timer_start_periodic(s_flush_timer, (uint64_t)FLUSH_INTERVAL_MS * 1000ULL);
            ESP_LOGI(TAG, "Flush timer iniciado (%d ms)", FLUSH_INTERVAL_MS);
        }
    }

    ESP_LOGI(TAG, "Datalogger iniciado (RAM %d entradas, SD %s)",
             DATALOGGER_MAX_ENTRIES, s_sd_mounted ? "OK" : "no disponible");
    return ESP_OK;
}

bool datalogger_is_ready(void) { return true; }

esp_err_t datalogger_log(const frigo_state_t *frigo)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    datalogger_entry_t entry;
    get_timestamp(entry.timestamp, sizeof(entry.timestamp));
    entry.T_Aletas     = frigo->T_Aletas;
    entry.T_Congelador = frigo->T_Congelador;
    entry.T_Exterior   = frigo->T_Exterior;
    entry.fan_percent  = frigo->fan_percent;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buf[s_head] = entry;
        s_head = (s_head + 1) % DATALOGGER_MAX_ENTRIES;
        if (s_count < DATALOGGER_MAX_ENTRIES) s_count++;
        if (s_pending_count < DATALOGGER_MAX_ENTRIES) {
            s_pending_count++;
        } else {
            /* buffer pendientes lleno: descartar mas antiguo */
            s_pending_first = (s_pending_first + 1) % DATALOGGER_MAX_ENTRIES;
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Log[%d]: %s | %.1f | %.1f | %.1f | fan=%d%%",
             s_count, entry.timestamp,
             entry.T_Aletas, entry.T_Congelador,
             entry.T_Exterior, entry.fan_percent);
    return ESP_OK;
}

int datalogger_get_count(void) { return s_count; }

const datalogger_entry_t *datalogger_get_entry(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    int real = (s_count < DATALOGGER_MAX_ENTRIES) ? index : (s_head + index) % DATALOGGER_MAX_ENTRIES;
    return &s_buf[real];
}

char *datalogger_get_csv(void)
{
    if (!s_mutex || s_count == 0) return NULL;
    size_t size = 80 + (size_t)s_count * 80;
    char *csv = malloc(size);
    if (!csv) return NULL;
    int pos = 0;
    pos += snprintf(csv + pos, size - pos,
                    "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < s_count && pos < (int)size - 80; i++) {
            const datalogger_entry_t *e = datalogger_get_entry(i);
            if (!e) continue;
            char ta[10], tc[10], te[10];
            format_temp(ta, sizeof ta, e->T_Aletas);
            format_temp(tc, sizeof tc, e->T_Congelador);
            format_temp(te, sizeof te, e->T_Exterior);
            pos += snprintf(csv + pos, size - pos,
                            "%s,%s,%s,%s,%d\n",
                            e->timestamp, ta, tc, te, e->fan_percent);
        }
        xSemaphoreGive(s_mutex);
    }
    return csv;
}
