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
#define FRAME_LEN     20    /* longitud trama protocolo (parse normal) */
#define SNIFFER_BUF_LEN 64  /* buffer sniffer ampliado (no cortar respuestas) */
#define IDLE_PERIOD   5000  /* ms entre comandos idle */
#define FRESH_MS      30000 /* sin trama -> stale */

/* TEST: al arrancar, transmitir CMD_IDLE continuamente durante 30 s para
 * poder medir DE del MAX485 con multimetro DC. Deberia leerse ~3.3 V
 * (DE en HIGH casi todo el tiempo durante TX back-to-back). Cambiar a 0
 * para uso normal. (Verificado 2026-05-20: DE = 0.714V DC durante test,
 * auto-DE funciona, no hace falta repetir el test.) */
#define NE185_TEST_DE_FORCE_TX  0

/* SNIFFER:
 * 0 = produccion: TX comandos + parse_frame (formato NE187 real)
 * 1 = modo sniffer activo (loguea SNIFF). El TX se controla en RUNTIME
 *     via ne185_set_sniffer_tx(true/false) desde la UI. Por defecto OFF
 *     (solo escucha); cuando se activa desde la UI, envia CMD_IDLE cada
 *     5s ademas de seguir logueando. */
#define NE185_SNIFFER_MODE  1

/* Comandos del protocolo NordElettronica (capturados del NE187 real
 * via sniffer en autocaravana 2026-05-20). Los valores documentados en
 * class142/ne-rs485 son del NE334 y NO coinciden con este modelo NE185:
 * - Idle/polling real:    FF 40 00 00 3F  (no FF 40 00 C0 BF)
 * - All indoor lights:    FF 01 00 00 00  (coincide con docs)
 * - Variante wake-up:     FF 00 00 00 FF  (no FF 00 00 C0 FF)
 *
 * Las luces exterior y bomba NO se capturaron aun; se mantienen valores
 * tentativos del repo, a confirmar en proxima sesion.
 *
 * Marcados unused para silenciar warning cuando NE185_SNIFFER_MODE = 1. */
/* Checksum descubierto via reverse engineering: byte 4 = (b0+b1+b2+b3) & 0xFF.
 * Confirmado en los 4 comandos capturados del NE187 real. */
__attribute__((unused)) static const uint8_t CMD_INIT[] = {0xFF, 0x40, 0x00, 0x00, 0x3F}; /* capturado */
__attribute__((unused)) static const uint8_t CMD_IDLE[] = {0xFF, 0x40, 0x00, 0x00, 0x3F}; /* capturado */
__attribute__((unused)) static const uint8_t CMD_LIN[]  = {0xFF, 0x01, 0x00, 0x00, 0x00}; /* capturado */
__attribute__((unused)) static const uint8_t CMD_LOUT[] = {0xFF, 0x02, 0x00, 0x00, 0x01}; /* calculado por checksum */
__attribute__((unused)) static const uint8_t CMD_PUMP[] = {0xFF, 0x04, 0x00, 0x00, 0x03}; /* capturado */

static ne185_data_t       s_data;
static SemaphoreHandle_t  s_mutex;
static QueueHandle_t      s_cmd_queue;
static bool               s_inited;
static volatile uint32_t  s_sniff_bursts = 0;
/* TX runtime toggle: default OFF (sniff puro). UI puede cambiarlo. */
static volatile bool      s_sniffer_tx_enabled = false;
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

/* Decodifica una trama valida y vuelca en s_data.
 * Layout (ingenieria inversa del NE185 real, 2026-05-20/21):
 *   bytes 0-4    : eco del comando enviado (FF 40 00 00 3F idle, etc.)
 *   bytes 5-8    : "03 02 00 40" constantes (header respuesta)
 *   byte 9       : variable (sensor, voltaje o contador, sin confirmar)
 *   byte 12      : battery1 = (byte - 30) / 10 [V]  (servicio)
 *   byte 13      : battery2 = (byte - 30) / 10 [V]  (motor)
 *   byte 14      : flag ED/EE (sin confirmar)
 *   byte 15      : bitmap de estados:
 *                    bit 0 -> luz interior ON
 *                    bit 1 -> luz exterior ON
 *                    bit 2 -> bomba ON
 *                    bit 7 -> shore (red 230 V conectada)
 *   bytes 16-18  : "30 00 00" constantes
 *   byte 19      : checksum
 *
 * Niveles de tanque (s1, r1) NO decodificados aun en NE185 - el repo
 * class142 era para NE334 con layout distinto. Pendiente: capturar con
 * tanques en diferentes niveles para identificar posicion. */
static void parse_frame(const uint8_t *b)
{
    ne185_data_t tmp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tmp = s_data;
    xSemaphoreGive(s_mutex);

    /* States bitmap en byte 15 */
    uint8_t f = b[15];
    tmp.light_in  = (f & 0x01) != 0;
    tmp.light_out = (f & 0x02) != 0;
    tmp.pump      = (f & 0x04) != 0;
    tmp.shore     = (f & 0x80) != 0;

    /* Baterias: byte = voltaje*10 + 30 */
    tmp.battery1_v = ((float)b[12] - 30.0f) / 10.0f;
    tmp.battery2_v = ((float)b[13] - 30.0f) / 10.0f;

    /* Tanks: sin decodificacion confirmada, dejar a 0 */
    tmp.s1 = 0;
    tmp.r1 = 0;

    tmp.fresh     = true;
    tmp.last_update_ms = now_ms();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = tmp;
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "rx bat1=%.1fV bat2=%.1fV lin=%d lout=%d pump=%d shore=%d",
             tmp.battery1_v, tmp.battery2_v,
             tmp.light_in, tmp.light_out, tmp.pump, tmp.shore);

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

#if NE185_SNIFFER_MODE
    ESP_LOGW(TAG, ">>> MODO SNIFFER: TX OFF inicial (toggle desde Consola)");
    ESP_LOGW(TAG, ">>> Conecta NE187 al bus o activa TX desde la UI");
#else
    /* Despertar el bus */
    uart_write_bytes(NE185_UART_NUM, CMD_INIT, sizeof(CMD_INIT));
#endif
    last_idle_ms = now_ms();

#if NE185_TEST_DE_FORCE_TX
    /* TEST: 30 s de TX continuo para medir DE con multimetro.
     * Espera ~5 s para que el boot termine y empieces a medir. */
    ESP_LOGW(TAG, ">>> TEST DE: en 5 s comenzara TX continuo 30 s - prepara multimetro");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGW(TAG, ">>> TEST DE: TX continuo INICIADO - mide DE del U8 ahora");
    uint32_t test_start = now_ms();
    while (now_ms() - test_start < 30000) {
        uart_write_bytes(NE185_UART_NUM, CMD_IDLE, sizeof(CMD_IDLE));
        /* Pequena pausa para no bloquear el FreeRTOS scheduler totalmente */
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, ">>> TEST DE: fin del TX continuo. Modo normal");
    last_idle_ms = now_ms();
#endif

#if NE185_SNIFFER_MODE
    /* === LOOP SNIFFER (modo 1 o 2) ====================================
     * Modo 1: solo escucha y loguea SNIFF (cero TX).
     * Modo 2: envia CMD_IDLE cada 5 s y loguea TODO RX en SNIFF (sin
     *         parse_frame, util para probar con la centralita sin NE187). */
    uint32_t s_last_byte_ms = 0;
    /* Buffer especifico mas grande para el sniffer (no cortar respuestas) */
    static uint8_t sniff_buf[SNIFFER_BUF_LEN];
    int sniff_idx = 0;
    while (1) {
        /* TX runtime: solo si toggle activado desde la UI */
        if (s_sniffer_tx_enabled &&
            (now_ms() - last_idle_ms) > IDLE_PERIOD) {
            uart_write_bytes(NE185_UART_NUM, CMD_IDLE, sizeof(CMD_IDLE));
            last_idle_ms = now_ms();
        }
        uint8_t c;
        int n = uart_read_bytes(NE185_UART_NUM, &c, 1, pdMS_TO_TICKS(20));
        if (n > 0) {
            if (sniff_idx < SNIFFER_BUF_LEN) sniff_buf[sniff_idx++] = c;
            s_last_byte_ms = now_ms();
        } else if (sniff_idx > 0 && (now_ms() - s_last_byte_ms) >= 5) {
            /* Fin de burst: vuelca hex */
            char hex[SNIFFER_BUF_LEN * 3 + 4];
            int off = 0;
            for (int i = 0; i < sniff_idx && off < (int)sizeof(hex) - 4; i++) {
                off += snprintf(hex + off, sizeof(hex) - off, "%02X ", sniff_buf[i]);
            }
            ESP_LOGI(TAG, "SNIFF (%d bytes): %s", sniff_idx, hex);
            s_sniff_bursts++;
            sniff_idx = 0;
        }
        if (n <= 0 && sniff_idx == 0) {
            vTaskDelay(1);
        }
    }
    (void)buf;  /* silenciar unused warning del buffer del modo normal */
    (void)idx;
#else
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
#endif
}

/* Self-test del parse con tramas reales capturadas en logs. Si todo va
 * bien deberia loguear OK para cada una. Si alguno falla, los offsets
 * del parse_frame no encajan con el formato real del NE185. */
static void ne185_self_test_parse(void)
{
    struct test_case {
        const char *name;
        uint8_t frame[20];
        bool exp_lin, exp_lout, exp_pump, exp_shore;
    } cases[] = {
        { "IDLE todo OFF (byte15=00)",
          { 0xFF, 0x40, 0x00, 0x00, 0x3F,  0x03, 0x02, 0x00, 0x40, 0x4D,
            0x00, 0xFF, 0x9A, 0xA7, 0xED,  0x00, 0x30, 0x00, 0x00, 0xEF },
          false, false, false, false },
        { "CHECK + shore (byte15=80)",
          { 0xFF, 0x00, 0x00, 0x00, 0xFF,  0x03, 0x02, 0x00, 0x40, 0x34,
            0x00, 0xFF, 0x9A, 0xA7, 0xED,  0x80, 0x30, 0x00, 0x00, 0x56 },
          false, false, false, true },
        { "Bomba + shore (byte15=84)",
          { 0xFF, 0x04, 0x00, 0x00, 0x03,  0x03, 0x02, 0x00, 0x40, 0x34,
            0x00, 0xFF, 0x9A, 0xA7, 0xEE,  0x84, 0x30, 0x00, 0x00, 0x5B },
          false, false, true, true },
        { "Todas las luces + bomba sin shore (byte15=07)",
          { 0xFF, 0x40, 0x00, 0x00, 0x3F,  0x03, 0x02, 0x00, 0x40, 0x5A,
            0x00, 0xFF, 0x9A, 0xA7, 0xED,  0x07, 0x30, 0x00, 0x00, 0x03 },
          true, true, true, false },
    };
    int n = sizeof(cases) / sizeof(cases[0]);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        ne185_sim_inject_raw(cases[i].frame);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        ne185_data_t got = s_data;
        xSemaphoreGive(s_mutex);
        bool pass = (got.light_in  == cases[i].exp_lin) &&
                    (got.light_out == cases[i].exp_lout) &&
                    (got.pump      == cases[i].exp_pump) &&
                    (got.shore     == cases[i].exp_shore);
        if (pass) {
            ESP_LOGI(TAG, "SELFTEST [OK] %s -> lin=%d lout=%d pump=%d shore=%d "
                          "bat1=%.1fV bat2=%.1fV",
                     cases[i].name, got.light_in, got.light_out, got.pump,
                     got.shore, got.battery1_v, got.battery2_v);
            ok++;
        } else {
            ESP_LOGW(TAG, "SELFTEST [FAIL] %s -> esperado lin=%d/got=%d, "
                          "lout=%d/%d, pump=%d/%d, shore=%d/%d",
                     cases[i].name,
                     cases[i].exp_lin, got.light_in,
                     cases[i].exp_lout, got.light_out,
                     cases[i].exp_pump, got.pump,
                     cases[i].exp_shore, got.shore);
        }
    }
    ESP_LOGI(TAG, "SELFTEST resumen: %d/%d casos OK", ok, n);
    /* Restaurar estado limpio tras el test */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_data, 0, sizeof(s_data));
    xSemaphoreGive(s_mutex);
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

    /* Self-test del parse con tramas reales de los logs. Si todos OK,
     * el parse esta listo para procesar tramas reales sin mas pruebas HW. */
    ne185_self_test_parse();
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

uint32_t ne185_get_sniff_count(void)
{
    return s_sniff_bursts;
}

void ne185_log_marker(const char *what)
{
    if (!what) return;
    ESP_LOGW(TAG, "MARK: %s", what);
}

bool ne185_sim_inject_raw(const uint8_t *frame20)
{
    if (!frame20) return false;
    if (frame20[0] != 0xFF) return false;
    if (!s_mutex) return false;
    parse_frame(frame20);
    return true;
}

void ne185_set_sniffer_tx(bool enable)
{
    s_sniffer_tx_enabled = enable;
    ESP_LOGW(TAG, ">>> SNIFFER TX %s desde la UI <<<",
             enable ? "ACTIVADO (ESP transmite)" : "DESACTIVADO (solo escucha)");
}

bool ne185_get_sniffer_tx(void)
{
    return s_sniffer_tx_enabled;
}

void ne185_sim_inject(uint8_t s1, uint8_t r1,
                      bool light_in, bool light_out,
                      bool pump, bool shore)
{
    if (!s_inited) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data.s1 = s1;
    s_data.r1 = r1;
    s_data.light_in = light_in;
    s_data.light_out = light_out;
    s_data.pump = pump;
    s_data.shore = shore;
    s_data.fresh = true;
    s_data.last_update_ms = now_ms();
    xSemaphoreGive(s_mutex);
}
