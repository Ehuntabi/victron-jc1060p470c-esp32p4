/* ne185.c — Maestro RS-485 directo */
#include "ne185.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ne185";

#define RX_BUF_SIZE   1024
#define TX_BUF_SIZE   256
#define FRAME_LEN     20    /* longitud del buffer ventana de sincronizacion */
#define IDLE_PERIOD   5000  /* ms entre comandos idle */
#define FRESH_MS      30000 /* sin trama -> stale */

/* Comandos del protocolo NordElettronica (familia NE185/NE334) */
static const uint8_t CMD_INIT[] = {0xFF, 0x40, 0x00, 0x80, 0xBF};
static const uint8_t CMD_IDLE[] = {0xFF, 0x40, 0x00, 0xC0, 0xBF};
static const uint8_t CMD_LIN[]  = {0xFF, 0x01, 0x00, 0xC0, 0xC0};
static const uint8_t CMD_LOUT[] = {0xFF, 0x02, 0x00, 0xC0, 0xC1};
static const uint8_t CMD_PUMP[] = {0xFF, 0x04, 0x00, 0xC0, 0xC3};

static ne185_data_t       s_data;
static SemaphoreHandle_t  s_mutex;
static QueueHandle_t      s_cmd_queue;
static bool               s_inited;
/* Marcado a true tras la primera trama valida; se usa para encolar
 * automaticamente las acciones de auto-encendido (Luz INT + Bomba) */
static bool               s_initial_actions_done;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Suma bytes 0..17 mod 128, comparada con (b[18..19] mod 128) - 2.
 * Heuristica del repo class142/ne-rs485 (ingenieria inversa NE334).
 *
 * Reescrita como (s+2) % 128 == recv % 128 para evitar underflow cuando
 * recv % 128 < 2 (que con la formula original rechazaria el ~1.6% de
 * tramas validas debido a aritmetica unsigned). Equivalente matematica. */
static bool checksum_ok(const uint8_t *b)
{
    uint16_t s = 0;
    for (int i = 0; i < FRAME_LEN - 2; i++) s += b[i];
    uint16_t recv = ((uint16_t)b[FRAME_LEN - 2] << 8) | b[FRAME_LEN - 1];
    return (((s + 2) % 128) == (recv % 128));
}

static int popcount3bit(uint8_t v)
{
    return __builtin_popcount(v & 0x07);
}

/* Decodifica una trama valida y vuelca en s_data. */
static void parse_frame(const uint8_t *b)
{
    /* Layout (segun reverse-engineering del repo class142/ne-rs485):
     *   byte 5 low nibble = nivel S1 (limpia)  bitmask 3 bits
     *   byte 6 low nibble = nivel R1 (grises 1)
     *   byte 7 low nibble = nivel R2 (grises 2, no expuesto)
     *   byte 15 low nibble = bitmask de salidas:
     *       bit 0 -> luz interior
     *       bit 1 -> luz exterior
     *       bit 2 -> bomba
     *   shore (red 230 V) -> bit 6 del mismo byte de flags
     */
    ne185_data_t tmp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tmp = s_data;
    xSemaphoreGive(s_mutex);

    tmp.s1 = popcount3bit(b[5]);
    tmp.r1 = popcount3bit(b[6]);
    uint8_t f = b[15];
    tmp.light_in  = (f & 0x01) != 0;
    tmp.light_out = (f & 0x02) != 0;
    tmp.pump      = (f & 0x04) != 0;
    tmp.shore     = (f & 0x40) != 0;
    tmp.fresh     = true;
    tmp.last_update_ms = now_ms();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = tmp;
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "rx s1=%u r1=%u lin=%d lout=%d pump=%d shore=%d",
             tmp.s1, tmp.r1, tmp.light_in, tmp.light_out, tmp.pump, tmp.shore);

    /* Acciones de auto-encendido al establecer conexion por primera vez:
     * Luz Interior y Bomba a ON si no lo estan. Los comandos son TOGGLE,
     * asi que solo se mandan si el estado actual es OFF. */
    if (!s_initial_actions_done) {
        s_initial_actions_done = true;
        if (!tmp.light_in) {
            char c = 'i';
            xQueueSend(s_cmd_queue, &c, 0);
            ESP_LOGI(TAG, "auto-init: Luz Interior -> ON");
        }
        if (!tmp.pump) {
            char c = 'p';
            xQueueSend(s_cmd_queue, &c, 0);
            ESP_LOGI(TAG, "auto-init: Bomba -> ON");
        }
    }
}

static void rs485_task(void *arg)
{
    (void)arg;
    uint8_t buf[FRAME_LEN];
    int idx = 0;
    uint32_t last_idle_ms = 0;

    /* Despertar el bus */
    uart_write_bytes(NE185_UART_NUM, CMD_INIT, sizeof(CMD_INIT));
    last_idle_ms = now_ms();

    while (1) {
        /* 1) Procesar comandos pendientes encolados desde la UI */
        char cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            const uint8_t *p = NULL;
            size_t n = 0;
            switch (cmd) {
                case 'i': p = CMD_LIN;  n = sizeof(CMD_LIN);  break;
                case 'o': p = CMD_LOUT; n = sizeof(CMD_LOUT); break;
                case 'p': p = CMD_PUMP; n = sizeof(CMD_PUMP); break;
            }
            if (p) {
                uart_write_bytes(NE185_UART_NUM, p, n);
                last_idle_ms = now_ms();
                ESP_LOGI(TAG, "tx cmd '%c'", cmd);
            }
        }

        /* 2) Mantener el bus despierto con idle cada IDLE_PERIOD */
        if (now_ms() - last_idle_ms > IDLE_PERIOD) {
            uart_write_bytes(NE185_UART_NUM, CMD_IDLE, sizeof(CMD_IDLE));
            last_idle_ms = now_ms();
        }

        /* 3) Leer respuesta del bus, byte a byte, con resincronizacion */
        uint8_t c;
        int n = uart_read_bytes(NE185_UART_NUM, &c, 1, pdMS_TO_TICKS(50));
        if (n > 0) {
            buf[idx++] = c;
            if (idx >= FRAME_LEN) {
                if (buf[0] == 0xFF && buf[14] == 0xFF && checksum_ok(buf)) {
                    parse_frame(buf);
                    idx = 0;
                } else {
                    /* shift left 1 byte para resincronizar */
                    for (int i = 0; i < FRAME_LEN - 1; i++) buf[i] = buf[i + 1];
                    idx = FRAME_LEN - 1;
                }
            }
        }

        /* 4) Marcar stale si llevamos mucho sin trama valida */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_data.fresh && (now_ms() - s_data.last_update_ms) > FRESH_MS) {
            s_data.fresh = false;
        }
        xSemaphoreGive(s_mutex);
    }
}

void ne185_init(void)
{
    if (s_inited) return;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el mutex; ne185 deshabilitado");
        return;
    }
    s_cmd_queue = xQueueCreate(8, sizeof(char));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "no se pudo crear la queue; ne185 deshabilitado");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return;
    }
    memset(&s_data, 0, sizeof(s_data));

    const uart_config_t cfg = {
        .baud_rate = NE185_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(NE185_UART_NUM, RX_BUF_SIZE,
                                        TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(NE185_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(NE185_UART_NUM, NE185_UART_TX, NE185_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(rs485_task, "ne185_rs485", 4096, NULL, 5, NULL);
    s_inited = true;
    ESP_LOGI(TAG,
             "RS-485 master listo (UART%d TX=%d RX=%d @ %d baud) — MAX485 onboard, conector J5",
             NE185_UART_NUM, NE185_UART_TX, NE185_UART_RX, NE185_UART_BAUD);
}

void ne185_get(ne185_data_t *out)
{
    if (!out || !s_inited) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    if (out->fresh && (now_ms() - out->last_update_ms) > FRESH_MS) {
        out->fresh = false;
        s_data.fresh = false;
    }
    xSemaphoreGive(s_mutex);
}

void ne185_send_cmd(char cmd)
{
    if (!s_inited || !s_cmd_queue) return;
    if (cmd != 'i' && cmd != 'o' && cmd != 'p') return;
    xQueueSend(s_cmd_queue, &cmd, 0);
}
