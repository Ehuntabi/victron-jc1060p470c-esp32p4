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
#include "watchdog.h"

static const char *TAG = "ne185";

#define RX_BUF_SIZE       1024
#define TX_BUF_SIZE       256
#define FRAME_LEN         20
#define POLL_PERIOD_MS    100  /* subido de 60->100 el 2026-05-27: dar mas
                                * tiempo al NE185 a procesar entre cmds. El
                                * bus tiene bias correcto (placa user con
                                * R1=R2=680 + R3=132 terminacion) pero TX
                                * a 60ms producia toggle erratico */
#define HOLD_FRAMES       8    /* subido de 4->8: mas margen para que >=2
                                * frames consecutivos lleguen limpios al
                                * NE185 (necesita 2 para procesar toggle). */
#define RELEASE_FRAMES    5    /* subido de 2->5: mas idle entre press del
                                * mismo boton para evitar doble toggle */
#define READ_TIMEOUT_MS   200  /* timeout lectura respuesta NE185 (NE185 puede tardar 50-150ms) */
#define FRESH_MS          30000
#define BUS_DEAD_THRESH   20   /* N timeouts consecutivos -> bus caido */
#define MUTEX_TIMEOUT_MS  100  /* timeout take mutex (en lugar de portMAX_DELAY)
                                * para detectar starvation con log en vez de
                                * bloquear la task indefinidamente */

/* Comandos NUEVA HIPOTESIS 2026-05-27 tras descubrir repos class142/ne-rs485
 * (NE334) + thespinmaster/venus-os (NE319/334). Lo que YO interprete como
 * "cmd del NE187" del sniffer (FF 40 00 00 3F y FF 4X) era en realidad el
 * ECHO de la respuesta del NE185 al NE187 (que incluye echo del cmd en
 * los primeros bytes del frame canonical 20). Los cmds REALES son:
 *
 *   IDLE: byte3=0x80, checksum=0xBF
 *   ACTION: byte3=0xC0, byte1 = bit del boton (0x01,0x02,0x04,0x08)
 *
 * Esto explica por que el NE185 sin NE187 nos respondia frame15 degradado
 * (7C E0 00 40...) cuando le mandabamos cmd con b3=0x00 - era invalido. */
static const uint8_t CMD_IDLE[]     = {0xFF, 0x40, 0x00, 0x80, 0xBF};
static const uint8_t CMD_BTN_LIN[]  = {0xFF, 0x01, 0x00, 0xC0, 0xC0};
static const uint8_t CMD_BTN_LOUT[] = {0xFF, 0x02, 0x00, 0xC0, 0xC1};
static const uint8_t CMD_BTN_PUMP[] = {0xFF, 0x04, 0x00, 0xC0, 0xC3};
/* Adicional: cmds aprendidos de los repos publicos (no usados aun pero
 * listos): CMD_BTN_AUX = FF 08 00 C0 C7, CMD_ALL_OFF = FF 80 00 00 7F */

/* Cmd init DESCARTADO 2026-05-27 19:30. Rompia el TX al NE185.
 * Conservamos comentado por si en el futuro lo necesitamos investigar.
 * static const uint8_t CMD_INIT[] = {0xFF, 0x40, 0x00, 0x80, 0xBF}; */

static ne185_data_t       s_data;
static SemaphoreHandle_t  s_mutex;
static QueueHandle_t      s_press_queue;
static bool               s_inited;
static volatile uint32_t  s_sniff_bursts = 0;
static volatile uint32_t  s_frames_fail = 0;
static volatile bool      s_verbose_log = true;   /* default ON 2026-05-27: fase reverse
                                                   * engineering del frame15 nativo,
                                                   * necesitamos capturar TODO el hex raw.
                                                   * Usuario puede apagarlo desde UI si
                                                   * el buffer satura otros logs */
static volatile bool      s_polling_paused = false; /* sniff mode: no envia, solo lee */
static volatile uint8_t   s_custom_cmd_b1 = 0;       /* 0 = nada pendiente */
static volatile bool      s_custom_cmd_set = false;
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
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "parse_frame: mutex starvation, skip");
        return;
    }
    prev = s_data;
    tmp  = s_data;
    xSemaphoreGive(s_mutex);

    /* Detectar frame15 nativo NE185 reconstruido: b[5..6] = 0x7C 0xE0 (header
     * propio del frame nativo, NO datos de tanks). En ese caso NO decodificar
     * tanks porque no estan en este formato corto - falso positivo de
     * "aguas grises llena" reportado por user 2026-05-27 19:08. */
    bool is_native_ne185 = (b[5] == 0x7C && b[6] == 0xE0);

    /* Estados bitmap (byte 15) */
    uint8_t f = b[15];
    tmp.light_in  = (f & 0x01) != 0;
    tmp.light_out = (f & 0x02) != 0;
    tmp.pump      = (f & 0x04) != 0;
    tmp.shore     = (f & 0x80) != 0;

    /* Baterias: voltaje = (byte - 30) / 10 */
    tmp.battery1_v = ((float)b[12] - 30.0f) / 10.0f;
    tmp.battery2_v = ((float)b[13] - 30.0f) / 10.0f;

    if (is_native_ne185) {
        /* Frame nativo NE185 (sin NE187): no contiene tanks - dejar
         * los valores actuales (preservar). Marcar como "sin dato". */
        tmp.s1 = 0xFF;
        tmp.r1 = 0xFF;
    } else {
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
    }

    tmp.fresh = true;
    tmp.last_update_ms = now_ms();

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "parse_frame commit: mutex starvation, skip");
        return;
    }
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
    #define WATCH_TIMEOUT_MS 1500          /* tras 1.5s sin cambio -> FAILED.
                                            * Margen para variabilidad RX si
                                            * la comunicacion se degrada */

    ESP_LOGI(TAG, "Master RS-485 NE185 activo (poll %d ms, hold %d frames)",
             POLL_PERIOD_MS, HOLD_FRAMES);

    /* INIT sequence DESACTIVADA 2026-05-27 19:30:
     * Hipotesis: el cmd CMD_INIT (FF 40 00 80 BF) ponia al NE185 en
     * un modo distinto que rechazaba los cmds posteriores. Evidencia:
     * tras este cambio, b[4] del frame15 paso de rango 0x40-0x58 a 0x79+
     * y el TX dejo de funcionar incluso erratically (antes funcionaba a
     * veces, ahora no funciona nunca).
     * Volvemos al comportamiento previo: solo poll FF 40 00 00 3F desde
     * el primer frame, sin init wake-up. */

    while (1) {
        watchdog_heartbeat(WD_TASK_NE185);
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
                /* Iniciar watch: recordar estado actual del bit, esperar cambio.
                 * Si starvation -> default false (mejor falso negativo que bloqueo). */
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    watch_prev_state = (s_data.light_in && watch_bit_mask == 0x01) ||
                                       (s_data.light_out && watch_bit_mask == 0x02) ||
                                       (s_data.pump && watch_bit_mask == 0x04);
                    xSemaphoreGive(s_mutex);
                } else {
                    ESP_LOGE(TAG, "press start: mutex starvation, asumiendo prev_state=false");
                    watch_prev_state = false;
                }
                if (watching_press) {
                    /* Anterior watcher no llego a CONFIRMED/FAILED; logueamos
                     * antes de sobreescribir para no perder el resultado. */
                    ESP_LOGW(TAG, "press '%c' watcher abortado tras %lu ms "
                                  "(llego nuevo '%c')",
                             watching_press, now_ms() - watch_start_ms,
                             current_press);
                }
                watching_press = current_press;
                watch_start_ms = now_ms();
                ESP_LOGI(TAG, "press '%c' start (was %s, expect toggle)",
                         current_press, watch_prev_state ? "ON" : "OFF");
            }
        }

        /* Watcher: comprobar si el press se confirmo o fallo */
        if (watching_press) {
            uint32_t elapsed = now_ms() - watch_start_ms;
            bool current_state = false;
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                current_state = (watch_bit_mask == 0x01) ? s_data.light_in :
                                (watch_bit_mask == 0x02) ? s_data.light_out :
                                (watch_bit_mask == 0x04) ? s_data.pump : false;
                xSemaphoreGive(s_mutex);
            } else {
                ESP_LOGE(TAG, "watcher: mutex starvation, skip ciclo");
                /* Saltamos esta evaluacion; reintentamos al proximo tick */
                goto watcher_skip;
            }
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
watcher_skip:;
        }

        /* === CUSTOM CMD pendiente? ===
         * Si se inyecto un cmd custom desde la UI (ne185_inject_custom_cmd),
         * lo enviamos UNA vez en lugar del cmd normal. Para diagnosticar
         * que responde el NE185 a otros cmds (FF 50, FF 60, etc.). */
        uint8_t custom_cmd_buf[5];
        if (s_custom_cmd_set) {
            uint8_t b1 = s_custom_cmd_b1;
            custom_cmd_buf[0] = 0xFF;
            custom_cmd_buf[1] = b1;
            custom_cmd_buf[2] = 0x00;
            custom_cmd_buf[3] = 0x00;
            custom_cmd_buf[4] = (0xFF + b1) & 0xFF;
            tx_cmd = custom_cmd_buf;
            tx_len = 5;
            ESP_LOGW(TAG, ">>> CUSTOM tx: FF %02X 00 00 %02X <<<",
                     b1, custom_cmd_buf[4]);
            s_custom_cmd_set = false;
            /* Cancelar press en curso si estabamos en hold */
            press_frames_left = 0;
            release_frames_left = 0;
        }

        /* === SNIFF MODE (pause polling) ===
         * Si s_polling_paused == true: no enviamos. Solo intentamos leer.
         * Util para detectar si el NE185 emite frames espontaneamente. */
        if (s_polling_paused) {
            int n = uart_read_bytes(NE185_UART_NUM, buf, FRAME_LEN,
                                    pdMS_TO_TICKS(READ_TIMEOUT_MS));
            if (n > 0) {
                if (n == 15) {
                    ESP_LOGI(TAG, "sniff rx15: %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X",
                             buf[0],  buf[1],  buf[2],  buf[3],  buf[4],
                             buf[5],  buf[6],  buf[7],  buf[8],  buf[9],
                             buf[10], buf[11], buf[12], buf[13], buf[14]);
                } else if (n == 20) {
                    ESP_LOGI(TAG, "sniff rx20: %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X",
                             buf[0],  buf[1],  buf[2],  buf[3],  buf[4],
                             buf[5],  buf[6],  buf[7],  buf[8],  buf[9],
                             buf[10], buf[11], buf[12], buf[13], buf[14],
                             buf[15], buf[16], buf[17], buf[18], buf[19]);
                } else {
                    ESP_LOGI(TAG, "sniff rx %d bytes b[0..4]=%02X %02X %02X %02X %02X",
                             n, buf[0], buf[1], buf[2], buf[3], buf[4]);
                }
            }
            vTaskDelayUntil(&last_tick, pdMS_TO_TICKS(POLL_PERIOD_MS));
            continue;
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
            /* Caso B: frame completo con echo (canonical NE187 sniff) */
            memcpy(frame20, buf, FRAME_LEN);
            ok = true;
        } else if (n == 15) {
            /* Caso A1: frame15 NATIVO NE185 (descubierto 2026-05-27).
             * Cabecera 7C E0 00 40, contador en b[4].
             *
             * 2026-05-27 19:08: comprobado que el checksum NO es simple
             * b[4] | 0xA0 como pensaba (solo coincidio por casualidad
             * en una muestra). Hace falta mas data para reverse-engineer
             * la formula real.
             *
             * Estrategia provisional: aceptar el frame solo por la
             * CABECERA (7C E0 00 40) sin validar checksum. La probabilidad
             * de que 4 bytes consecutivos coincidan por ruido en el bus
             * es 1/(2^32) - despreciable. Si el frame es corrupto, los
             * bytes parsed (bat, bitmap) seran "raros" pero no peligrosos.
             *
             * Reconstruimos canonical tx_cmd+buf para que parse_frame
             * extraiga lo decodificable (bat servicio buf[7], bat motor
             * buf[8], bitmap buf[10]). */
            /* Aceptar TODAS las variantes de header observadas hasta ahora.
             * El NE185 responde con headers distintos segun el cmd recibido:
             *   - 7C E0: cmd legacy FF 40 00 00 3F (sniffer NE187 historico)
             *   - FC E0: variante transitoria 0.4% de frames (poco frecuente)
             *   - F8 E0: cmd moderno FF 40 00 80 BF (descubierto 2026-05-28)
             *
             * 2026-05-28 user reporto que con cmd nuevo el LED de la UI no se
             * encendia. El header cambio a F8 E0 (bit 2 vs 7C tiene bit 2 ON
             * y bit 7 OFF; F8 tiene bit 7 ON y bit 2 OFF; FC tiene ambos ON).
             *
             * Solucion mas robusta: aceptar cualquier b[0] con bit 6 set (todos
             * los observados tienen bit 6 = 0x40 set: 7C=...111100, FC=...111100,
             * F8=...111000 todos). Filtramos por bits 0-3 (b1 must be E0)
             * y b[1]=0xE0 + b[2]=0x00 + (b[3] 0x40 or 0x00). */
            if ((buf[0] & 0x40) != 0 &&        /* bit 6 set en TODOS los headers observados */
                buf[1] == 0xE0 &&
                buf[2] == 0x00 &&
                (buf[3] == 0x40 || buf[3] == 0x00)) {
                memcpy(frame20, tx_cmd, 5);
                memcpy(frame20 + 5, buf, 15);
                ok = true;
            } else {
                /* Caso A2: respuesta canonical sin echo (fallback antiguo).
                 * Reconstruir con tx_cmd y validar checksum NE187. */
                memcpy(frame20, tx_cmd, 5);
                memcpy(frame20 + 5, buf, 15);
                if (checksum_ok(frame20)) {
                    ok = true;
                }
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
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    s_data.fresh = false;
                    xSemaphoreGive(s_mutex);
                } else {
                    ESP_LOGE(TAG, "marcar stale: mutex starvation");
                }
            }
            /* Log detallado del fallo. n<0 = error UART, n==0 = timeout puro
             * (no logueamos, ya hay "bus inactivo"), n>0 = bytes recibidos.
             *
             * CRITICO (2026-05-27): NE185 responde con bytes estructurados
             * que NO coinciden con la fórmula checksum del sniffer NE187
             * (7C E0 00 40 counter ...). Loguear TODOS los bytes para
             * reverse-engineer el nuevo formato. Una vez identificado,
             * volver a un log conciso. */
            if (n < 0) {
                ESP_LOGE(TAG, "uart_read_bytes error rc=%d", n);
            } else if (n > 0) {
                if (n == 15) {
                    ESP_LOGI(TAG, "rx15: %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X",
                             buf[0],  buf[1],  buf[2],  buf[3],  buf[4],
                             buf[5],  buf[6],  buf[7],  buf[8],  buf[9],
                             buf[10], buf[11], buf[12], buf[13], buf[14]);
                } else if (n == 20) {
                    ESP_LOGI(TAG, "rx20: %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X",
                             buf[0],  buf[1],  buf[2],  buf[3],  buf[4],
                             buf[5],  buf[6],  buf[7],  buf[8],  buf[9],
                             buf[10], buf[11], buf[12], buf[13], buf[14],
                             buf[15], buf[16], buf[17], buf[18], buf[19]);
                } else {
                    ESP_LOGI(TAG, "rx %d bytes (parcial). b[0..4]=%02X %02X %02X %02X %02X",
                             n, buf[0], buf[1], buf[2], buf[3], buf[4]);
                }
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
    if (xQueueSend(s_press_queue, &cmd, 0) != pdTRUE) {
        /* Cola llena (16 presses pendientes). Indica que el bus esta
         * caido o muy lento y el user esta pulsando mas rapido de lo que
         * podemos procesar. Press se descarta. */
        ESP_LOGW(TAG, "press '%c' descartado (queue full)", cmd);
    }
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

void ne185_set_polling_paused(bool paused)
{
    s_polling_paused = paused;
    ESP_LOGW(TAG, ">>> POLLING %s <<<", paused ? "PAUSED (sniff mode)" : "ACTIVE");
}

bool ne185_get_polling_paused(void)
{
    return s_polling_paused;
}

void ne185_inject_custom_cmd(uint8_t b1)
{
    s_custom_cmd_b1 = b1;
    s_custom_cmd_set = true;
    ESP_LOGW(TAG, ">>> custom cmd encolado: FF %02X 00 00 %02X <<<",
             b1, (0xFF + b1) & 0xFF);
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
