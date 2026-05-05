#include "rtc_rx8025t.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "RTC_RX8025T";

/* ── Constantes ──────────────────────────────────────────────── */
#define RTC_I2C_PORT     I2C_NUM_0
#define RTC_I2C_SCL      GPIO_NUM_10
#define RTC_I2C_SDA      GPIO_NUM_12
#define RTC_I2C_ADDR     0x64
#define RTC_I2C_CLK_HZ   50000

/* Registros RX8025T */
#define REG_SEC    0x00
#define REG_MIN    0x01
#define REG_HOUR   0x02
#define REG_WDAY   0x03
#define REG_MDAY   0x04
#define REG_MONTH  0x05
#define REG_YEAR   0x06
#define REG_CTRL1  0x0E
#define REG_CTRL2  0x0F

static i2c_master_bus_handle_t  s_bus    = NULL;
static i2c_master_dev_handle_t  s_dev    = NULL;

/* ── BCD helpers ─────────────────────────────────────────────── */
static inline uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

/* ── Leer N registros desde dirección reg ────────────────────── */
static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr = reg << 4;   /* RX8025T: dirección en bits [7:4] */
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_dev, &addr, 1, buf, len, 100),
        TAG, "i2c read failed");
    return ESP_OK;
}

/* ── Escribir un registro ────────────────────────────────────── */
static esp_err_t rtc_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { (uint8_t)(reg << 4), val };
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(s_dev, buf, 2, 100),
        TAG, "i2c write failed");
    return ESP_OK;
}

/* ── API pública ─────────────────────────────────────────────── */
esp_err_t rtc_init(void)
{
    /* Bus I2C_NUM_0 */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = RTC_I2C_PORT,
        .scl_io_num        = RTC_I2C_SCL,
        .sda_io_num        = RTC_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "bus init");


    /* Dispositivo RX8025T */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = RTC_I2C_CLK_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "dev add");

    /* Limpiar flag de error en CTRL2 si lo hay */
    rtc_write(REG_CTRL2, 0x00);

    ESP_LOGI(TAG, "RX8025T inicializado (I2C_NUM_0, SCL=GPIO%d, SDA=GPIO%d)",
             RTC_I2C_SCL, RTC_I2C_SDA);
    return ESP_OK;
}

esp_err_t rtc_get_time(struct tm *tm_out)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t regs[7];
    ESP_RETURN_ON_ERROR(rtc_read(REG_SEC, regs, 7), TAG, "get_time read");

    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_sec  = bcd2dec(regs[0] & 0x7F);
    tm_out->tm_min  = bcd2dec(regs[1] & 0x7F);
    tm_out->tm_hour = bcd2dec(regs[2] & 0x3F);
    tm_out->tm_wday = regs[3] & 0x07;
    tm_out->tm_mday = bcd2dec(regs[4] & 0x3F);
    tm_out->tm_mon  = bcd2dec(regs[5] & 0x1F) - 1;  /* 0-11 */
    tm_out->tm_year = bcd2dec(regs[6]) + 100;         /* desde 1900 */
    return ESP_OK;
}

esp_err_t rtc_set_time(const struct tm *tm_in)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    rtc_write(REG_SEC,   dec2bcd(tm_in->tm_sec));
    rtc_write(REG_MIN,   dec2bcd(tm_in->tm_min));
    rtc_write(REG_HOUR,  dec2bcd(tm_in->tm_hour));
    rtc_write(REG_WDAY,  (1 << tm_in->tm_wday));
    rtc_write(REG_MDAY,  dec2bcd(tm_in->tm_mday));
    rtc_write(REG_MONTH, dec2bcd(tm_in->tm_mon + 1));
    rtc_write(REG_YEAR,  dec2bcd(tm_in->tm_year - 100));
    ESP_LOGI(TAG, "Hora ajustada: %04d-%02d-%02d %02d:%02d:%02d",
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
