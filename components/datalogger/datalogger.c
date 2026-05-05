#include "datalogger.h"
#include "rtc_rx8025t.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "DATALOGGER";

#define MOUNT_POINT   "/sdcard"
#define SD_CLK        GPIO_NUM_36
#define SD_CMD        GPIO_NUM_35
#define SD_D0         GPIO_NUM_37
#define SD_D1         GPIO_NUM_38
#define SD_D2         GPIO_NUM_39
#define SD_D3         GPIO_NUM_40

static bool           s_ready = false;
static sdmmc_card_t  *s_card  = NULL;

esp_err_t datalogger_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SD_CLK;
    slot.cmd   = SD_CMD;
    slot.d0    = SD_D0;
    slot.d1    = SD_D1;
    slot.d2    = SD_D2;
    slot.d3    = SD_D3;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.cd = GPIO_NUM_NC;  /* sin pin card detect */
    slot.wp = GPIO_NUM_NC;  /* sin write protect */

    esp_err_t ret = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "slot0 init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 400;  /* 400 KHz, mínimo absoluto */
    host.flags        = SDMMC_HOST_FLAG_1BIT; /* 1-bit mode */
    

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_ready = true;
    ESP_LOGI(TAG, "SD montada en %s (SDMMC slot0, 4-bit)", MOUNT_POINT);
    return ESP_OK;
}

bool datalogger_is_ready(void) { return s_ready; }

static void get_timestamp(char *ts_buf, size_t ts_len,
                           char *date_buf, size_t date_len)
{
    struct tm t;
    if (rtc_get_time(&t) == ESP_OK && t.tm_year > 100) {
        strftime(ts_buf,   ts_len,   "%Y-%m-%d %H:%M:%S", &t);
        strftime(date_buf, date_len, "%Y%m%d",             &t);
    } else {
        uint64_t ms = esp_timer_get_time() / 1000;
        uint32_t s  = (uint32_t)(ms / 1000);
        uint32_t h  = s / 3600; s %= 3600;
        uint32_t m  = s / 60;   s %= 60;
        snprintf(ts_buf,   ts_len,   "BOOT+%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
        snprintf(date_buf, date_len, "00000000");
    }
}

static void ensure_header(FILE *f, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        fprintf(f, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n");
}

esp_err_t datalogger_log(const frigo_state_t *frigo)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char ts[32], date[12];
    get_timestamp(ts, sizeof(ts), date, sizeof(date));

    char path[48];
    snprintf(path, sizeof(path), MOUNT_POINT "/LOG_%s.csv", date);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s", path);
        return ESP_FAIL;
    }

    ensure_header(f, path);

    char ta[10], tc[10], te[10];
    if (frigo->T_Aletas     < -120.0f) strcpy(ta, "---");
    else snprintf(ta, sizeof(ta), "%.1f", frigo->T_Aletas);
    if (frigo->T_Congelador < -120.0f) strcpy(tc, "---");
    else snprintf(tc, sizeof(tc), "%.1f", frigo->T_Congelador);
    if (frigo->T_Exterior   < -120.0f) strcpy(te, "---");
    else snprintf(te, sizeof(te), "%.1f", frigo->T_Exterior);

    fprintf(f, "%s,%s,%s,%s,%d\n", ts, ta, tc, te, frigo->fan_percent);
    fclose(f);

    ESP_LOGI(TAG, "Log: %s | %s | %s | %s | fan=%d%%",
             ts, ta, tc, te, frigo->fan_percent);
    return ESP_OK;
}
