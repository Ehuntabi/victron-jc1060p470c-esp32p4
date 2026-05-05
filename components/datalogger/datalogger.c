#include "datalogger.h"
#include "rtc_rx8025t.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "DATALOGGER";

#define MOUNT_POINT   "/sdcard"
#define SD_CLK        GPIO_NUM_36
#define SD_MOSI       GPIO_NUM_35
#define SD_MISO       GPIO_NUM_37
#define SD_CS         GPIO_NUM_40
#define SPI_HOST      SPI2_HOST

static bool           s_ready = false;
static sdmmc_card_t  *s_card  = NULL;

esp_err_t datalogger_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));  /* esperar alimentación SD */

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST;

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = SD_CS;
    dev_cfg.host_id = SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI_HOST);
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_ready = true;
    ESP_LOGI(TAG, "SD montada en %s (SPI2)", MOUNT_POINT);
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

static void ensure_header(FILE *f)
{
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
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

    ensure_header(f);

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
