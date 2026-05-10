#include "rtc_rx8025t.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RTC_RX8025T";

#define RTC_I2C_ADDR   0x32
#define RTC_I2C_CLK_HZ 100000

/* Registros RX8025T (Epson) */
#define REG_SEC        0x00
#define REG_MIN        0x01
#define REG_HOUR       0x02
#define REG_WDAY       0x03
#define REG_MDAY       0x04
#define REG_MONTH      0x05
#define REG_YEAR       0x06
#define REG_RAM        0x07
#define REG_ALARM_MIN  0x08
#define REG_ALARM_HOUR 0x09
#define REG_ALARM_WD   0x0A
#define REG_TC0        0x0B
#define REG_TC1        0x0C
#define REG_EXT        0x0D
#define REG_FLAG       0x0E
#define REG_CONTROL    0x0F
#define REG_OFFSET     0x10

/* CONTROL bits */
#define CTRL_RESET     0x01
#define CTRL_AIE       0x08
#define CTRL_TIE       0x10
#define CTRL_UIE       0x20
#define CTRL_STOP      0x40   /* 1 = oscilador parado */
#define CTRL_TEST      0x80

/* FLAG bits */
#define FLAG_VLF       0x02   /* Voltage Low Flag */
#define FLAG_VDET      0x01
#define FLAG_AF        0x08
#define FLAG_TF        0x10
#define FLAG_UF        0x20

static i2c_master_dev_handle_t s_dev   = NULL;
static bool                    s_ready = false;

static inline uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t rtc_write(uint8_t reg, uint8_t val)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

esp_err_t rtc_init(i2c_master_bus_handle_t bus)
{
    s_ready = false;

    if (!bus) {
        ESP_LOGE(TAG, "Bus I2C no valido");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_master_probe(bus, RTC_I2C_ADDR, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T no encontrado en 0x%02X", RTC_I2C_ADDR);
        ESP_LOGW(TAG, "Verificar pila CR1220");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "RX8025T encontrado en 0x%02X", RTC_I2C_ADDR);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = RTC_I2C_CLK_HZ,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dev add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Limpiar VLF (Voltage Low Flag) si está activo. Si lo está, la hora del
     * RTC no es fiable. Indicamos ESP_OK pero el caller debe verificar año. */
    uint8_t flag = 0;
    if (rtc_read(REG_FLAG, &flag, 1) == ESP_OK && (flag & FLAG_VLF)) {
        ESP_LOGW(TAG, "VLF activo — RTC perdió la hora; limpiando flag");
        rtc_write(REG_FLAG, flag & ~FLAG_VLF);
    }

    s_ready = true;
    ESP_LOGI(TAG, "RX8025T OK (addr=0x%02X)", RTC_I2C_ADDR);
    return ESP_OK;
}

bool rtc_is_ready(void) { return s_ready; }

esp_err_t rtc_get_time(struct tm *tm_out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t regs[7];

    esp_err_t ret = rtc_read(REG_SEC, regs, 7);
    if (ret != ESP_OK) return ret;

    /* Validar BCD; reintentar una vez si están fuera de rango */
    if ((regs[0] & 0x7F) > 0x59 || (regs[1] & 0x7F) > 0x59 ||
        (regs[2] & 0x3F) > 0x23) {
        ret = rtc_read(REG_SEC, regs, 7);
        if (ret != ESP_OK) return ret;
    }

    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_sec  = bcd2dec(regs[0] & 0x7F);
    tm_out->tm_min  = bcd2dec(regs[1] & 0x7F);
    tm_out->tm_hour = bcd2dec(regs[2] & 0x3F);
    /* WEEK = bitmap (1<<wday). Convertimos a wday 0-6. */
    uint8_t wmap = regs[3] & 0x7F;
    int wday = 0;
    for (int i = 0; i < 7; i++) { if (wmap & (1u << i)) { wday = i; break; } }
    tm_out->tm_wday = wday;
    tm_out->tm_mday = bcd2dec(regs[4] & 0x3F);
    tm_out->tm_mon  = bcd2dec(regs[5] & 0x1F) - 1;
    tm_out->tm_year = bcd2dec(regs[6]) + 100;  /* siglo 21 (2000-2099) */
    return ESP_OK;
}

esp_err_t rtc_set_time(const struct tm *tm_in)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    /* Detener oscilador antes de escribir (RX8025T: CONTROL bit 6 = STOP) */
    uint8_t ctrl = 0;
    rtc_read(REG_CONTROL, &ctrl, 1);
    rtc_write(REG_CONTROL, ctrl | CTRL_STOP);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t buf[8];
    buf[0] = REG_SEC;
    buf[1] = dec2bcd(tm_in->tm_sec);
    buf[2] = dec2bcd(tm_in->tm_min);
    buf[3] = dec2bcd(tm_in->tm_hour);
    buf[4] = (uint8_t)(1u << (tm_in->tm_wday & 0x07));
    buf[5] = dec2bcd(tm_in->tm_mday);
    buf[6] = dec2bcd(tm_in->tm_mon + 1);
    buf[7] = dec2bcd(tm_in->tm_year - 100);

    esp_err_t ret = i2c_master_transmit(s_dev, buf, 8, 100);

    /* Reanudar oscilador (limpiar STOP) y limpiar VLF que se pudo activar */
    rtc_write(REG_CONTROL, ctrl & ~CTRL_STOP);
    uint8_t flag = 0;
    if (rtc_read(REG_FLAG, &flag, 1) == ESP_OK && (flag & FLAG_VLF)) {
        rtc_write(REG_FLAG, flag & ~FLAG_VLF);
    }

    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Hora escrita: %04d-%02d-%02d %02d:%02d:%02d",
             tm_in->tm_year + 1900, tm_in->tm_mon + 1, tm_in->tm_mday,
             tm_in->tm_hour, tm_in->tm_min, tm_in->tm_sec);
    return ESP_OK;
}

time_t rtc_get_timestamp(void)
{
    struct tm t;
    if (rtc_get_time(&t) != ESP_OK) return 0;
    return mktime(&t);
}
