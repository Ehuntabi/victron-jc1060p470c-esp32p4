/* ne185.c — Master RS-485 NE185 (refactor 2026-05-26)
 *
 * Cambios respecto a la version anterior (basados en analisis de logs reales
 * del NE187 original capturados 2026-05-21):
 *
 *  1. CMD FORMAT CORREGIDO. NE187 envia botones como overlay sobre IDLE:
 *        IDLE     = FF 40 00 00 3F
 *        LUZ INT  = FF 41 00 00 40  (no FF 01 que no existe en el protocolo)
 *        LUZ EXT  = FF 42 00 00 41  (no FF 02)
 *        BOMBA    = FF 44 00 00 43  (no FF 04, por eso la bomba nunca funciono)
 *     byte1 = 0x40 | <bit boton>, byte4 = (b0+b1+b2+b3) & 0xFF
 *
 *  2. PRESS HOLD. NE185 ignora un solo frame FF 4X; necesita >=2 frames
 *     consecutivos a 60ms para procesar el toggle. NE187 los envia mientras
 *     el usuario tiene el dedo en el boton (~4 frames = 240ms).
 *
 *  3. CADENCIA 60ms. Igual que NE187 (16Hz). La UI ve cambios al instante.
 *
 *  4. LOOP SINCRONO. cmd -> wait_tx_done -> read 20 bytes -> parse. Sin race.
 *
 *  5. CHECKSUM CORREGIDO (derivado de tramas reales):
 *        b19 = (b5 + b9 + b14 + b15 + 0xB1) & 0xFF
 *     La formula anterior era del repo class142 (NE334), no aplicaba.
 *
 *  6. Quitado check buf[14] == 0xFF (byte 14 es sensor variable, no marcador).
 *
 *  7. parse_frame SIEMPRE se llama. La version anterior en modo sniffer solo
 *     loguea hex y no actualizaba s_data -> LED de los botones nunca encendia.
 */
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

#define RX_BUF_SIZE       1024
#define TX_BUF_SIZE       256
#define FRAME_LEN         20
#define POLL_PERIOD_MS    60   /* cadencia igual a NE187 real (16Hz) */
#define HOLD_FRAMES       4    /* frames consecutivos FF 4X por press (~240ms) */
#define RELEASE_FRAMES    2    /* frames IDLE entre press y press del mismo boton */
#define READ_TIMEOUT_MS   200  /* timeout lectura respuesta NE185 (NE185 puede tardar 50-150ms) */
#define FRESH_MS          30000
#define BUS_DEAD_THRESH   20   /* N timeouts consecutivos -> bus caido */

/* Comandos (verificados con tramas reales del NE187, ver checksum.py) */
static const uint8_t CMD_IDLE[]     = {0xFF, 0x40, 0x00, 0x00, 0x3F};
static const uint8_t CMD_BTN_LIN[]  = {0xFF, 0x41, 0x00, 0x00, 0x40};
static const uint8_t CMD_BTN_LOUT[] = {0xFF, 0x42, 0x00, 0x00, 0x41};
static const uint8_t CMD_BTN_PUMP[] = {0xFF, 0x44, 0x00, 0x00, 0x43};

static ne185_data_t       s_data;
static SemaphoreHandle_t  s_mutex;
static QueueHandle_t      s_press_queue;
static bool               s_inited;
static volatile uint32_t  s_sniff_bursts = 0;
static volatile uint32_t  s_frames_fail = 0;
static volatile bool      s_verbose_log = false;  /* default OFF - activar desde UI "LOG ON" durante test
                                                   * (16Hz logueando rellena el buffer RAM rapido) */
static uint8_t            s_last_raw[FRAME_LEN];
static SemaphoreHandle_t  s_raw_mutex;            /* protege s_last_raw */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Checksum derivado de tramas reales del NE185:
 *   b[19] = (b[5] + b[9] + b[14] + b[15] + 0xB1) & 0xFF
 * Validado en 5+ tramas distintas (IDLE, FF41, FF42, FF44 responses). */
static bool checksum_ok(const uint8_t *b)
{
    uint8_t exp = (b[5] + b[9] + b[14] + b[15] + 0xB1) & 0xFF;
    return exp == b[19];
}

/* Decodifica trama valida y vuelca en s_data.
 * Layout (autopsia tramas reales NE185, 2026-05-21/26):
 *   0..4  : eco del cmd enviado
 *   5     : nibble bajo = nivel tanque LIMPIO (0/1/3/7/F -> 0/1/2/3/4 cuartos)
 *   6     : tanque GRISES (bit 1 set = vacio, 0 = lleno per observacion user)
 *   7..8  : 00 40 constantes
 *   9     : variable (counter/sensor, sin confirmar - logueado en verbose)
 *   10    : 00 constante
 *   11    : FF constante
 *   12    : battery1 servicio  V = (byte - 30) / 10
 *   13    : battery2 motor     V = (byte - 30) / 10
 *   14    : variable (sensor/temperatura?, sin confirmar - logueado en verbose)
 *   15    : bitmap estados: bit0=lin, bit1=lout, bit2=pump, bit7=shore
 *   16..18: 30 00 00 constantes
 *   19    : checksum
 */
static void parse_frame(const uint8_t *b)
{
    ne185_data_t prev;
    ne185_data_t tmp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    prev = s_data;
    tmp  = s_data;
    xSemaphoreGive(s_mutex);

    /* Estados bitmap (byte 15) */
    uint8_t f = b[15];
    tmp.light_in  = (f & 0x01) != 0;
    tmp.light_out = (f & 0x02) != 0;
    tmp.pump      = (f & 0x04) != 0;
    tmp.shore     = (f & 0x80) != 0;

    /* Baterias: voltaje = (byte - 30) / 10 */
    tmp.battery1_v = ((float)b[12] - 30.0f) / 10.0f;
    tmp.battery2_v = ((float)b[13] - 30.0f) / 10.0f;

    /* Tanque LIMPIO (clean): nibble bajo de byte 5
     *   0x0 = Reserva (vacio)
     *   0x1 = 1/4
     *   0x3 = 2/4
     *   0x7 = 3/4
     *   0xF = 4/4 (lleno)
     *   otro = 0xFF (combo invalido / sin datos)
     */
    uint8_t raw_clean = b[5] & 0x0F;
    switch (raw_clean) {
        case 0x0: tmp.s1 = 0;    break;
        case 0x1: tmp.s1 = 1;    break;
        case 0x3: tmp.s1 = 2;    break;
        case 0x7: tmp.s1 = 3;    break;
        case 0xF: tmp.s1 = 4;    break;
        default:  tmp.s1 = 0xFF; break;
    }

    /* Tanque GRISES: bit 1 de byte 6 (user observo 0x02 con tanque vacio).
     * Hipotesis: probe se moja cuando lleno -> bit 1 a 0; seco (vacio) -> bit 1 a 1.
     *   r1 = 0 vacio, 1 lleno. */
    tmp.r1 = (b[6] & 0x02) ? 0 : 1;

    tmp.fresh = true;
    tmp.last_update_ms = now_ms();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = tmp;
    xSemaphoreGive(s_mutex);

    /* Log de cambios de estado (no de cada frame, evita spam a 60ms) */
    bool state_change = (prev.light_in  != tmp.light_in)  ||
                        (prev.light_out != tmp.light_out) ||
                        (prev.pump      != tmp.pump)      ||
                        (prev.shore     != tmp.shore)     ||
                        (prev.s1        != tmp.s1)        ||
                        (prev.r1        != tmp.r1)        ||
                        !prev.fresh;
    if (state_change) {
        ESP_LOGI(TAG, "state lin=%d lout=%d pump=%d shore=%d clean=%d grey=%d "
                      "bat1=%.1fV bat2=%.1fV",
                 tmp.light_in, tmp.light_out, tmp.pump, tmp.shore,
                 tmp.s1, tmp.r1, tmp.battery1_v, tmp.battery2_v);
    }
    if (s_verbose_log) {
        ESP_LOGI(TAG, "raw b5=%02X b6=%02X b9=%02X b14=%02X b15=%02X chk=%02X",
                 b[5], b[6], b[9], b[14], b[15], b[19]);
    }
}

static void rs485_task(void *arg)
{
    (void)arg;
    uint8_t buf[FRAME_LEN];

    /* Estado del press hold */
    char current_press = 0;          /* 0 = idle, 'i'/'o'/'p' = pulsando */
    int  press_frames_left = 0;       /* frames FF 4X restantes en hold actual */
    int  release_frames_left = 0;     /* frames IDLE de margen tras release */
    uint32_t consec_timeouts = 0;
    TickType_t last_tick = xTaskGetTickCount();

    /* Tracking para "press confirmado": al iniciar press, recordamos el bit
     * del estado AL QUE deberia cambiar (toggle). Tras release esperamos
     * unos frames y comprobamos si el bit cambio. Si si -> CONFIRMED.
     * Si no tras N frames -> FAILED. */
    char     watching_press = 0;          /* boton 'i'/'o'/'p' pendiente de confirmar */
    uint32_t watch_start_ms = 0;          /* ts cuando empezo el watch */
    uint8_t  watch_bit_mask = 0;          /* mascara byte 15 del bit que debe cambiar */
    bool     watch_prev_state = false;    /* estado antes del press */
    #define WATCH_TIMEOUT_MS 800           /* tras 800 ms sin cambio -> FAILED */

    ESP_LOGI(TAG, "Master RS-485 NE185 activo (poll %d ms, hold %d frames)",
             POLL_PERIOD_MS, HOLD_FRAMES);

    while (1) {
        /* === FSM de cmd a enviar ====================================== */
        const uint8_t *tx_cmd = CMD_IDLE;
        size_t tx_len = sizeof(CMD_IDLE);

        if (press_frames_left > 0) {
            /* Continuar hold actual */
            switch (current_press) {
                case 'i': tx_cmd = CMD_BTN_LIN;  tx_len = sizeof(CMD_BTN_LIN);  break;
                case 'o': tx_cmd = CMD_BTN_LOUT; tx_len = sizeof(CMD_BTN_LOUT); break;
                case 'p': tx_cmd = CMD_BTN_PUMP; tx_len = sizeof(CMD_BTN_PUMP); break;
            }
            press_frames_left--;
            if (press_frames_left == 0) {
                /* Acabamos hold, ahora release */
                release_frames_left = RELEASE_FRAMES;
                ESP_LOGI(TAG, "press '%c' release", current_press);
                current_press = 0;
            }
        } else if (release_frames_left > 0) {
            /* En release: forzar IDLE */
            release_frames_left--;
        } else {
            /* Buscar siguiente press en la cola, coalesciendo duplicados */
            char cmd;
            while (xQueueReceive(s_press_queue, &cmd, 0) == pdTRUE) {
                if (cmd == 'i' || cmd == 'o' || cmd == 'p') {
                    current_press = cmd;
                    press_frames_left = HOLD_FRAMES;
                    /* Drenar duplicados consecutivos del MISMO boton */
                    char peek;
                    while (xQueuePeek(s_press_queue, &peek, 0) == pdTRUE && peek == cmd) {
                        xQueueReceive(s_press_queue, &peek, 0);
                    }
                    break;
                }
            }
            if (current_press) {
                switch (current_press) {
                    case 'i': tx_cmd = CMD_BTN_LIN;  tx_len = sizeof(CMD_BTN_LIN);
                              watch_bit_mask = 0x01; break;
                    case 'o': tx_cmd = CMD_BTN_LOUT; tx_len = sizeof(CMD_BTN_LOUT);
                              watch_bit_mask = 0x02; break;
                    case 'p': tx_cmd = CMD_BTN_PUMP; tx_len = sizeof(CMD_BTN_PUMP);
                              watch_bit_mask = 0x04; break;
                }
                press_frames_left--;
                /* Iniciar watch: recordar estado actual del bit, esperar cambio */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                watch_prev_state = (s_data.light_in && watch_bit_mask == 0x01) ||
                                   (s_data.light_out && watch_bit_mask == 0x02) ||
                                   (s_data.pump && watch_bit_mask == 0x04);
                xSemaphoreGive(s_mutex);
                watching_press = current_press;
                watch_start_ms = now_ms();
                ESP_LOGI(TAG, "press '%c' start (was %s, expect toggle)",
                         current_press, watch_prev_state ? "ON" : "OFF");
            }
        }

        /* Watcher: comprobar si el press se confirmo o fallo */
        if (watching_press) {
            uint32_t elapsed = now_ms() - watch_start_ms;
            bool current_state;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            current_state = (watch_bit_mask == 0x01) ? s_data.light_in :
                            (watch_bit_mask == 0x02) ? s_data.light_out :
                            (watch_bit_mask == 0x04) ? s_data.pump : false;
            xSemaphoreGive(s_mutex);
            if (current_state != watch_prev_state) {
                ESP_LOGI(TAG, "press '%c' CONFIRMED in %lu ms (%s -> %s)",
                         watching_press, elapsed,
                         watch_prev_state ? "ON" : "OFF",
                         current_state ? "ON" : "OFF");
                watching_press = 0;
            } else if (elapsed > WATCH_TIMEOUT_MS) {
                ESP_LOGW(TAG, "press '%c' FAILED (no toggle tras %lu ms, sigue %s)",
                         watching_press, elapsed,
                         watch_prev_state ? "ON" : "OFF");
                watching_press = 0;
            }
        }

        /* === TX + RX sync ============================================ */
        /* NO flush antes: respuesta del NE185 puede llegar muy rapido
         * y un flush descartaria bytes legitimos. */
        uart_write_bytes(NE185_UART_NUM, tx_cmd, tx_len);
        uart_wait_tx_done(NE185_UART_NUM, pdMS_TO_TICKS(20));

        /* Leemos hasta 20 bytes con timeout largo. Dos modos posibles:
         *  - Caso A: auto-DE bloquea RX durante TX -> solo llegan 15 bytes
         *            de respuesta NE185 (sin echo del cmd).
         *  - Caso B: auto-DE imperfecto -> llegan 20 bytes (5 echo + 15 resp).
         * Manejamos ambos.
         *
         * uart_read_bytes con timeout 200ms espera hasta FRAME_LEN(20) o
         * timeout. Si llegan 15 bytes y se queda esperando 5 mas, agota
         * el timeout y devuelve n=15. */
        int n = uart_read_bytes(NE185_UART_NUM, buf, FRAME_LEN,
                                pdMS_TO_TICKS(READ_TIMEOUT_MS));

        bool ok = false;
        uint8_t frame20[FRAME_LEN];

        if (n == FRAME_LEN && buf[0] == 0xFF && checksum_ok(buf)) {
            /* Caso B: frame completo con echo */
            memcpy(frame20, buf, FRAME_LEN);
            ok = true;
        } else if (n == 15) {
            /* Caso A: solo respuesta (15 bytes). Reconstruir frame de 20
             * prependiendo el cmd que enviamos (tx_cmd) en bytes 0..4. */
            memcpy(frame20, tx_cmd, 5);
            memcpy(frame20 + 5, buf, 15);
            if (checksum_ok(frame20)) {
                ok = true;
            }
        }

        if (ok) {
            parse_frame(frame20);
            if (s_raw_mutex) {
                xSemaphoreTake(s_raw_mutex, portMAX_DELAY);
                memcpy(s_last_raw, frame20, FRAME_LEN);
                xSemaphoreGive(s_raw_mutex);
            }
            s_sniff_bursts++;
            consec_timeouts = 0;
        } else {
            consec_timeouts++;
            s_frames_fail++;
            if (consec_timeouts == BUS_DEAD_THRESH) {
                ESP_LOGW(TAG, "bus inactivo (%lu timeouts) - marcando stale",
                         consec_timeouts);
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_data.fresh = false;
                xSemaphoreGive(s_mutex);
            }
            /* Log detallado del fallo (LOGI para que aparezca en log SD) */
            if (n > 0) {
                ESP_LOGI(TAG, "rx %d bytes (esperaba 15 o 20). b[0..4]=%02X %02X %02X %02X %02X",
                         n, buf[0], buf[1], buf[2], buf[3], buf[4]);
            }
        }

        /* Mantener cadencia exacta de POLL_PERIOD_MS */
        vTaskDelayUntil(&last_tick, pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* Self-test con tramas reales capturadas del NE187 (log_20260521_*). */
static void ne185_self_test(void)
{
    struct test_case {
        const char *name;
        uint8_t frame[20];
        bool exp_lin, exp_lout, exp_pump, exp_shore;
    } cases[] = {
        { "IDLE response, todo OFF",
          { 0xFF, 0x40, 0x00, 0x00, 0x3F, 0x03, 0x02, 0x00, 0x40, 0x5F,
            0x00, 0xFF, 0x9A, 0xA6, 0xED, 0x00, 0x30, 0x00, 0x00, 0x00 },
          false, false, false, false },
        { "FF41 response, luz INT ON",
          { 0xFF, 0x41, 0x00, 0x00, 0x40, 0x03, 0x02, 0x00, 0x40, 0x66,
            0x00, 0xFF, 0x9A, 0xA6, 0xED, 0x01, 0x30, 0x00, 0x00, 0x08 },
          true, false, false, false },
        { "FF42 response, luz EXT ON",
          { 0xFF, 0x42, 0x00, 0x00, 0x41, 0x03, 0x02, 0x00, 0x40, 0x60,
            0x00, 0xFF, 0x9A, 0xA6, 0xED, 0x02, 0x30, 0x00, 0x00, 0x03 },
          false, true, false, false },
        { "FF44 response, EXT + BOMBA",
          { 0xFF, 0x44, 0x00, 0x00, 0x43, 0x01, 0x02, 0x00, 0x40, 0x69,
            0x00, 0xFF, 0x9A, 0xA6, 0xEC, 0x06, 0x30, 0x00, 0x00, 0x0D },
          false, true, true, false },
    };
    int n = sizeof(cases) / sizeof(cases[0]);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (!checksum_ok(cases[i].frame)) {
            ESP_LOGE(TAG, "SELFTEST [FAIL chk] %s", cases[i].name);
            continue;
        }
        ne185_sim_inject_raw(cases[i].frame);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        ne185_data_t got = s_data;
        xSemaphoreGive(s_mutex);
        bool pass = (got.light_in  == cases[i].exp_lin)  &&
                    (got.light_out == cases[i].exp_lout) &&
                    (got.pump      == cases[i].exp_pump) &&
                    (got.shore     == cases[i].exp_shore);
        if (pass) {
            ESP_LOGI(TAG, "SELFTEST [OK] %s -> lin=%d lout=%d pump=%d shore=%d "
                          "bat1=%.1fV bat2=%.1fV clean=%d grey=%d",
                     cases[i].name, got.light_in, got.light_out, got.pump,
                     got.shore, got.battery1_v, got.battery2_v, got.s1, got.r1);
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
    if (!s_mutex) {
        ESP_LOGE(TAG, "no se pudo crear mutex; ne185 deshabilitado");
        return;
    }
    s_press_queue = xQueueCreate(16, sizeof(char));
    if (!s_press_queue) {
        ESP_LOGE(TAG, "no se pudo crear queue; ne185 deshabilitado");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return;
    }
    s_raw_mutex = xSemaphoreCreateMutex();
    memset(&s_data, 0, sizeof(s_data));
    memset(s_last_raw, 0, sizeof(s_last_raw));

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

    ne185_self_test();
    ESP_LOGI(TAG,
             "NE185 master listo (UART%d TX=%d RX=%d @ %d baud, poll %d ms)",
             NE185_UART_NUM, NE185_UART_TX, NE185_UART_RX, NE185_UART_BAUD,
             POLL_PERIOD_MS);
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
    if (!s_inited || !s_press_queue) return;
    if (cmd != 'i' && cmd != 'o' && cmd != 'p') return;
    xQueueSend(s_press_queue, &cmd, 0);
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

/* Verbose log toggle: si ON, loguea hex de cada frame RX (util para
 * diagnosticar cambios en bytes desconocidos b9/b14). Default OFF para
 * evitar spam a 16Hz. */
void ne185_set_verbose(bool enable)
{
    s_verbose_log = enable;
    ESP_LOGW(TAG, ">>> VERBOSE log %s <<<", enable ? "ON (log raw RX)" : "OFF");
}

bool ne185_get_verbose(void)
{
    return s_verbose_log;
}

void ne185_get_last_raw(uint8_t out[FRAME_LEN], uint32_t *n_frames_ok,
                        uint32_t *n_frames_fail)
{
    if (out && s_raw_mutex) {
        xSemaphoreTake(s_raw_mutex, portMAX_DELAY);
        memcpy(out, s_last_raw, FRAME_LEN);
        xSemaphoreGive(s_raw_mutex);
    }
    if (n_frames_ok)   *n_frames_ok   = s_sniff_bursts;
    if (n_frames_fail) *n_frames_fail = s_frames_fail;
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
