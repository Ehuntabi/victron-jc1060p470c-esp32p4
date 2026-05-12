/* camper.c — implementacion */
#include "camper.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "camper";

#define CAMPER_LINE_MAX  256
#define RX_BUF_SIZE      1024
#define TX_BUF_SIZE      256
#define FRESH_MS         5000

static camper_data_t        s_data;
static SemaphoreHandle_t    s_mutex;
static camper_on_update_cb_t s_cb;
static bool                 s_inited;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void parse_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ESP_LOGW(TAG, "JSON invalido: %s", line);
        return;
    }
    camper_data_t tmp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tmp = s_data;
    xSemaphoreGive(s_mutex);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "s1"))   && cJSON_IsNumber(v)) tmp.s1        = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "r1"))   && cJSON_IsNumber(v)) tmp.r1        = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "lin"))  && cJSON_IsNumber(v)) tmp.light_in  = v->valueint != 0;
    if ((v = cJSON_GetObjectItem(root, "lout")) && cJSON_IsNumber(v)) tmp.light_out = v->valueint != 0;
    if ((v = cJSON_GetObjectItem(root, "pump")) && cJSON_IsNumber(v)) tmp.pump      = v->valueint != 0;
    if ((v = cJSON_GetObjectItem(root, "shore"))&& cJSON_IsNumber(v)) tmp.shore     = v->valueint != 0;
    tmp.fresh = true;
    tmp.last_update_ms = now_ms();
    cJSON_Delete(root);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = tmp;
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "rx s1=%u r1=%u lin=%d lout=%d pump=%d shore=%d",
             tmp.s1, tmp.r1, tmp.light_in, tmp.light_out, tmp.pump, tmp.shore);

    if (s_cb) s_cb();
}

static void uart_task(void *arg)
{
    (void)arg;
    char line[CAMPER_LINE_MAX];
    int  pos = 0;
    uint8_t byte;

    while (1) {
        int n = uart_read_bytes(CAMPER_UART_NUM, &byte, 1, pdMS_TO_TICKS(500));
        if (n <= 0) {
            /* timeout: marcar como stale si llevamos > FRESH_MS sin tramas */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_data.fresh &&
                (now_ms() - s_data.last_update_ms) > FRESH_MS) {
                s_data.fresh = false;
            }
            xSemaphoreGive(s_mutex);
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                parse_line(line);
                pos = 0;
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)byte;
        } else {
            /* desbordamiento: descartar linea */
            pos = 0;
        }
    }
}

void camper_init(void)
{
    if (s_inited) return;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el mutex; camper deshabilitado");
        return;
    }
    memset(&s_data, 0, sizeof(s_data));
    s_data.fresh = false;

    const uart_config_t cfg = {
        .baud_rate = CAMPER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CAMPER_UART_NUM,
                                        RX_BUF_SIZE, TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CAMPER_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAMPER_UART_NUM,
                                 CAMPER_UART_TX, CAMPER_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_task, "camper_uart", 4096, NULL, 5, NULL);
    s_inited = true;
    ESP_LOGI(TAG, "camper UART%d listo (TX=%d RX=%d @ %d baud)",
             CAMPER_UART_NUM, CAMPER_UART_TX, CAMPER_UART_RX,
             CAMPER_UART_BAUD);
}

void camper_get(camper_data_t *out)
{
    if (!out || !s_inited) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    /* re-evaluar fresh aqui tambien por si la UI lee sin que la tarea
     * haya pasado por timeout aun */
    if (out->fresh && (now_ms() - out->last_update_ms) > FRESH_MS) {
        out->fresh = false;
        s_data.fresh = false;
    }
    xSemaphoreGive(s_mutex);
}

void camper_send_cmd(char cmd)
{
    if (!s_inited) return;
    if (cmd != 'i' && cmd != 'o' && cmd != 'p') {
        ESP_LOGW(TAG, "cmd ignorado: '%c'", cmd);
        return;
    }
    uart_write_bytes(CAMPER_UART_NUM, &cmd, 1);
    ESP_LOGI(TAG, "tx cmd '%c'", cmd);
}

void camper_set_on_update_cb(camper_on_update_cb_t cb)
{
    s_cb = cb;
}
