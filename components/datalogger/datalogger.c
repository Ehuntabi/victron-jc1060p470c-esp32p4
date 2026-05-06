#include "datalogger.h"
#include "rtc_rx8025t.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "DATALOGGER";

static datalogger_entry_t s_buf[DATALOGGER_MAX_ENTRIES];
static int                s_head  = 0;
static int                s_count = 0;
static SemaphoreHandle_t  s_mutex = NULL;

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

esp_err_t datalogger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_head  = 0;
    s_count = 0;
    ESP_LOGI(TAG, "Datalogger iniciado (buffer RAM %d entradas)", DATALOGGER_MAX_ENTRIES);
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
            if (e->T_Aletas     < -120.0f) strcpy(ta, "---");
            else snprintf(ta, sizeof(ta), "%.1f", e->T_Aletas);
            if (e->T_Congelador < -120.0f) strcpy(tc, "---");
            else snprintf(tc, sizeof(tc), "%.1f", e->T_Congelador);
            if (e->T_Exterior   < -120.0f) strcpy(te, "---");
            else snprintf(te, sizeof(te), "%.1f", e->T_Exterior);
            pos += snprintf(csv + pos, size - pos,
                            "%s,%s,%s,%s,%d\n",
                            e->timestamp, ta, tc, te, e->fan_percent);
        }
        xSemaphoreGive(s_mutex);
    }
    return csv;
}
