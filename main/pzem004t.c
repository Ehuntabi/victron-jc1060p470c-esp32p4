#include "pzem004t.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "PZEM";

static struct {
    pzem_config_t cfg;
    pzem_data_t   data;
    int64_t       last_ok_us;
    SemaphoreHandle_t mtx;
    TaskHandle_t  task;
    bool          installed;
} s;

/* CRC16 Modbus: poly 0xA001 reflejado, init 0xFFFF */
static uint16_t crc16_modbus(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

/* Compone una petición Modbus "Read Input Registers" (func 0x04) y la envía.
 * Lee 25 bytes (slave + func + count + 20 data + 2 crc = 25). */
static esp_err_t pzem_poll_once(void)
{
    uint8_t req[8];
    req[0] = s.cfg.slave_address;
    req[1] = 0x04;             /* Read Input Registers */
    req[2] = 0x00; req[3] = 0x00;  /* start register = 0x0000 */
    req[4] = 0x00; req[5] = 0x0A;  /* 10 registros */
    uint16_t crc = crc16_modbus(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    /* Flush input antes de transmitir */
    uart_flush_input(s.cfg.uart_num);
    int wlen = uart_write_bytes(s.cfg.uart_num, (const char *)req, sizeof(req));
    if (wlen != sizeof(req)) return ESP_FAIL;

    uint8_t resp[25];
    int rlen = uart_read_bytes(s.cfg.uart_num, resp, sizeof(resp),
                               pdMS_TO_TICKS(500));
    if (rlen < (int)sizeof(resp)) {
        ESP_LOGD(TAG, "Timeout/incompleto: %d bytes", rlen);
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != s.cfg.slave_address || resp[1] != 0x04 || resp[2] != 20) {
        ESP_LOGW(TAG, "Frame invalido (addr=0x%02x func=0x%02x cnt=%u)",
                 resp[0], resp[1], resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t rx_crc = (uint16_t)resp[23] | ((uint16_t)resp[24] << 8);
    if (rx_crc != crc16_modbus(resp, 23)) {
        ESP_LOGW(TAG, "CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    /* Parse: cada registro big-endian. Algunos campos son 32-bit con el
     * registro de "low word" primero y luego "high word" (segun datasheet). */
    const uint8_t *d = &resp[3];
    #define R16(i) ((uint16_t)d[(i)*2] << 8 | d[(i)*2 + 1])
    uint16_t voltage    = R16(0);            /* 0.1 V */
    uint32_t current    = R16(1) | ((uint32_t)R16(2) << 16); /* 0.001 A */
    uint32_t power      = R16(3) | ((uint32_t)R16(4) << 16); /* 0.1 W */
    uint32_t energy     = R16(5) | ((uint32_t)R16(6) << 16); /* 1 Wh */
    uint16_t freq       = R16(7);            /* 0.1 Hz */
    uint16_t pf         = R16(8);            /* 0.01 */
    uint16_t alarm_st   = R16(9);            /* 0xFFFF = alarma */
    #undef R16

    /* Plausibilidad: aunque la trama tenga CRC valido, una corrupcion
     * cambia ~1/65536. Rechazamos rangos imposibles para PZEM-004T v3.
     * Limites laxos para no perder brown-outs reales (UE: 230 V nominal
     * pero un grupo electrogeno arrancando puede caer a 50 V) ni 110 V
     * single-phase en sag (~70 V). */
    float v_v   = voltage / 10.0f;
    float c_a   = current / 1000.0f;
    float p_w   = power   / 10.0f;
    float f_hz  = freq    / 10.0f;
    float pf_v  = pf      / 100.0f;
    bool plausible = (v_v >= 0.0f  && v_v <= 300.0f) &&
                     (c_a >= 0.0f  && c_a <= 100.0f) &&
                     (p_w >= 0.0f  && p_w <= 25000.0f) &&
                     (f_hz == 0.0f || (f_hz >= 40.0f && f_hz <= 70.0f)) &&
                     (pf_v >= 0.0f && pf_v <= 1.0f);
    if (!plausible) {
        ESP_LOGW(TAG, "Frame fuera de rango: V=%.1f C=%.3f P=%.1f F=%.1f PF=%.2f",
                 v_v, c_a, p_w, f_hz, pf_v);
        return ESP_ERR_INVALID_RESPONSE;
    }

    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.data.voltage_v    = v_v;
    s.data.current_a    = c_a;
    s.data.power_w      = p_w;
    s.data.energy_wh    = energy;
    s.data.freq_hz      = f_hz;
    s.data.power_factor = pf_v;
    s.data.alarm        = (alarm_st == 0xFFFF);
    s.data.has_data     = true;
    s.last_ok_us        = esp_timer_get_time();
    s.data.total_reads++;
    xSemaphoreGive(s.mtx);
    return ESP_OK;
}

static void pzem_task(void *arg)
{
    (void)arg;
    int consecutive_fail = 0;
    TickType_t period = pdMS_TO_TICKS(s.cfg.poll_period_ms);
    while (1) {
        esp_err_t err = pzem_poll_once();
        if (err == ESP_OK) {
            consecutive_fail = 0;
        } else {
            consecutive_fail++;
            xSemaphoreTake(s.mtx, portMAX_DELAY);
            s.data.failed_reads++;
            /* Si llevamos > 5 fallos seguidos, asumimos modulo desconectado y
             * marcamos has_data=false para que la UI muestre "--". El polling
             * continua por si se conecta despues. */
            if (consecutive_fail > 5) s.data.has_data = false;
            xSemaphoreGive(s.mtx);
        }
        vTaskDelay(period);
    }
}

esp_err_t pzem_init(const pzem_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s.installed) return ESP_ERR_INVALID_STATE;
    s.cfg = *cfg;
    if (s.cfg.slave_address == 0)  s.cfg.slave_address = 0x01;
    if (s.cfg.poll_period_ms == 0) s.cfg.poll_period_ms = 2000;
    if (s.mtx == NULL) s.mtx = xSemaphoreCreateMutex();

    uart_config_t uc = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s.cfg.uart_num, &uc));
    ESP_ERROR_CHECK(uart_set_pin(s.cfg.uart_num, s.cfg.tx_gpio, s.cfg.rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(s.cfg.uart_num, 256, 0, 0, NULL, 0));

    BaseType_t ok = xTaskCreate(pzem_task, "pzem", 2048, NULL, 4, &s.task);
    if (ok != pdPASS) return ESP_FAIL;
    s.installed = true;
    ESP_LOGI(TAG, "Init UART%d TX=%d RX=%d addr=0x%02x period=%lums",
             s.cfg.uart_num, s.cfg.tx_gpio, s.cfg.rx_gpio,
             s.cfg.slave_address, (unsigned long)s.cfg.poll_period_ms);
    return ESP_OK;
}

void pzem_get(pzem_data_t *out)
{
    if (!out) return;
    if (!s.installed || !s.mtx) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    *out = s.data;
    if (s.data.has_data && s.last_ok_us > 0) {
        out->age_ms = (uint32_t)((esp_timer_get_time() - s.last_ok_us) / 1000);
    } else {
        out->age_ms = 0;
    }
    xSemaphoreGive(s.mtx);
}
