#include "rtc_rx8025t.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "RTC_RX8130";

#define RTC_I2C_ADDR   0x32
#define RTC_I2C_CLK_HZ 100000

/* Registros RX8130 */
#define REG_SEC    0x10
#define REG_MIN    0x11
#define REG_HOUR   0x12
#define REG_WDAY   0x13
#define REG_MDAY   0x14
#define REG_MONTH  0x15
#define REG_YEAR   0x16
#define REG_FLAG   0x1D

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

    /* Verificar que el RX8130 responde */
    esp_err_t ret = i2c_master_probe(bus, RTC_I2C_ADDR, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX8130 no encontrado en 0x%02X", RTC_I2C_ADDR);
        ESP_LOGW(TAG, "Verificar pila CR1220");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "RX8130 encontrado en 0x%02X", RTC_I2C_ADDR);

    /* Añadir dispositivo al bus existente */
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

    /* Limpiar flags de error */
    rtc_write(REG_FLAG, 0x00);

    s_ready = true;
    ESP_LOGI(TAG, "RX8130 OK (addr=0x%02X, bus compartido con GT911)", RTC_I2C_ADDR);
    return ESP_OK;
}

bool rtc_is_ready(void) { return s_ready; }
esp_err_t rtc_get_time(struct tm *tm_out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t regs[7];

    /* Leer los 7 registros de una sola transacción I2C */
    esp_err_t ret = rtc_read(REG_SEC, regs, 7);
    if (ret != ESP_OK) return ret;

    /* Validar que los valores BCD son razonables */
    if ((regs[0] & 0x7F) > 0x59 || (regs[1] & 0x7F) > 0x59 ||
        (regs[2] & 0x3F) > 0x23) {
        /* Reintento */
        ret = rtc_read(REG_SEC, regs, 7);
        if (ret != ESP_OK) return ret;
    }

    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_sec  = bcd2dec(regs[0] & 0x7F);
    tm_out->tm_min  = bcd2dec(regs[1] & 0x7F);
    tm_out->tm_hour = bcd2dec(regs[2] & 0x3F);
    tm_out->tm_wday = regs[3] & 0x07;
    tm_out->tm_mday = bcd2dec(regs[4] & 0x3F);
    tm_out->tm_mon  = bcd2dec(regs[5] & 0x1F) - 1;
    tm_out->tm_year = bcd2dec(regs[6]) + 100;
    return ESP_OK;
}

esp_err_t rtc_set_time(const struct tm *tm_in)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    rtc_write(REG_SEC,   dec2bcd(tm_in->tm_sec));
    rtc_write(REG_MIN,   dec2bcd(tm_in->tm_min));
    rtc_write(REG_HOUR,  dec2bcd(tm_in->tm_hour));
    rtc_write(REG_WDAY,  (1 << tm_in->tm_wday));
    rtc_write(REG_MDAY,  dec2bcd(tm_in->tm_mday));
    rtc_write(REG_MONTH, dec2bcd(tm_in->tm_mon + 1));
    rtc_write(REG_YEAR,  dec2bcd(tm_in->tm_year - 100));
    ESP_LOGI(TAG, "Hora: %04d-%02d-%02d %02d:%02d:%02d",
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
