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
#include "config_storage.h"

static const char *TAG = "ne185";

#define RX_BUF_SIZE       1024
#define TX_BUF_SIZE       256
#define FRAME_LEN         20
#define RX_READ_LEN       40   /* 2026-06-23: leer ~2 frames para sincronizar.
                                * El read coge bytes en fase arbitraria del stream;
                                * leyendo 40 (>=34) garantizamos un frame nativo
                                * completo en cualquier offset -> el escaneo lo
                                * encuentra. Antes con 20 la cabecera rara vez caia
                                * en offset 0 y se descartaba ~39 de cada 40. */
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
/* POLL REAL del NE187 (sniff limpio 2026-06-24): el NE187 alterna
 * FF 40 00 00 3F y FF 00 00 00 FF; ambos provocan la misma respuesta de 15B
 * con tanques. Usamos FF 40 00 00 3F. El anterior FF 40 00 80 BF (b3=0x80)
 * era la "correccion" erronea de mayo que rompio la lectura. */
static const uint8_t CMD_IDLE[]     = {0xFF, 0x40, 0x00, 0x00, 0x3F};
/* Segundo poll que el NE187 manda (dominante). Hipotesis 2026-06-24: el NE185
 * solo entrega los tanques (trama 01 02...) si ve AMBOS polls alternados como
 * el NE187; con FF40 solo, degrada a F8 E0 sin tanques. Replicamos la alternancia. */
static const uint8_t CMD_IDLE2[]    = {0xFF, 0x00, 0x00, 0x00, 0xFF};
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

/* Auto-encendido de cargas al arranque (luz int + bomba). Ver ne185.h. */
static volatile bool      s_autostart_enabled = false; /* leido de NVS en init */
static bool               s_autostart_done    = false; /* one-shot por wake */
static uint32_t           s_good_frames       = 0;     /* tramas OK desde wake */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Checksum REAL del NE185 (descubierto 2026-06-24 por sniff limpio del poll
 * del NE187, con NE187 conectado como master y ESP en sniff):
 *   b[19] = (suma de b[5..18]) & 0xFF
 * Verificado en multiples tramas: clean=01 02 00 40 ED 00 FF 9C AB ED 81 31
 * 00 00 -> 0x15; con EC EC -> 0x13; con b15=01 -> 0x92. La formula anterior
 * (b5+b9+b14+b15+0xB1) era erronea -> ningun RESP pasaba el checksum y en
 * master se caia a la trama nativa degradada (sin tanques). */
static bool checksum_ok(const uint8_t *b)
{
    /* Constante estructural del frame valido: descarta tramas PARASITAS
     * (F8 E0 y derivadas) que aparecen en master y por azar cuadran el
     * checksum -> daban b6=0xFF, bat=-3.0V y parpadeo. b6=0x02 confirmado en
     * 2056 tramas y en el verbose (buena b6=02 / basura b6=FF). NOTA: NO usar
     * b11 aqui - en master no siempre es 0xFF y rechazaba las tramas buenas
     * (regresion 2026-06-24: 'no hay datos camper'). */
    if (b[6] != 0x02) return false;
    uint16_t sum = 0;
    for (int i = 5; i <= 18; i++) sum += b[i];
    return (uint8_t)sum == b[19];
}

/* Decodifica trama valida y vuelca en s_data.
 * Layout (autopsia tramas reales NE185, 2026-05-21/26):
 *   0..4  : eco del cmd enviado
 *   5     : nibble bajo = nivel tanque LIMPIO (0/1/3/7/F -> 0/1/2/3/4 cuartos)
 *   6     : constante 0x02 (NO es grises - hipotesis previa descartada 2026-06-23)
 *   7     : tanque GRISES R1 (0x00 vacio, 0x01 lleno). b8=0x40 constante
 *   9     : variable (counter/sensor, sin confirmar - logueado en verbose)
 *   10    : 00 constante
 *   11    : FF constante
 *   12    : battery1 servicio  V = (byte - 30) / 10
 *   13    : battery2 motor     V = (byte - 30) / 10
 *   14    : variable (sensor/temperatura?, sin confirmar - logueado en verbose)
 *   15    : bitmap estados: bit0=lin, bit1=lout, bit2=pump
 *           bit7 = heartbeat del poll (alterna con cmd FF00/FF40, NO es shore)
 *   16    : bit0 = shore 230V conectado (0x31 red / 0x30 sin red).
 *           Validado 2026-06-22 con test diferencial enchufar/desenchufar 220V.
 *   17..18: 00 00 constantes
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

    /* Estados bitmap (byte 15): luces y bomba.
     * El bit7 de b[15] NO es shore (es un heartbeat que alterna con el poll
     * FF00/FF40); el shore real esta en b[16] bit0 (validado 2026-06-22). */
    uint8_t f = b[15];
    tmp.light_in  = (f & 0x01) != 0;
    tmp.light_out = (f & 0x02) != 0;
    tmp.pump      = (f & 0x04) != 0;
    tmp.shore     = (b[16] & 0x01) != 0;

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

        /* Tanque GRISES R1: byte 7. Confirmado 2026-06-23 con test diferencial
         * (puente JP7 pin1<->pin2 = FULL): b7=0x00 vacio, b7=0x01 lleno. Doble
         * verificado (303 tramas b7=01 con puente, 0 sin el). El b6 (=0x02
         * constante) era hipotesis erronea y nunca llegaba a marcar lleno.
         *   r1 = 0 vacio, 1 lleno. */
        tmp.r1 = (b[7] & 0x01) ? 1 : 0;
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
}

static void rs485_task(void *arg)
{
    (void)arg;
    uint8_t buf[RX_READ_LEN];

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
        /* Idle poll: replica la alternancia del NE187. Manda FF 00 00 00 FF
         * (dominante) y FF 40 00 00 3F 1 de cada 16. Si solo FF40, el NE185
         * degrada a F8 E0 sin tanques (visto 2026-06-24). */
        static uint32_t s_idle_cnt = 0;
        const uint8_t *tx_cmd;
        size_t tx_len;
        if ((s_idle_cnt++ % 16) == 0) { tx_cmd = CMD_IDLE;  tx_len = sizeof(CMD_IDLE); }
        else                          { tx_cmd = CMD_IDLE2; tx_len = sizeof(CMD_IDLE2); }

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
        int n = uart_read_bytes(NE185_UART_NUM, buf, RX_READ_LEN,
                                pdMS_TO_TICKS(READ_TIMEOUT_MS));

        bool ok = false;
        uint8_t frame20[FRAME_LEN];

        /* Sincronizacion de frame por ESCANEO (2026-06-23).
         *
         * Problema: el read coge RX_READ_LEN bytes en una fase arbitraria del
         * stream continuo; la cabecera del frame casi nunca cae en offset 0.
         * Antes solo se aceptaba n==20 con echo FF o n==15 limpio, asi que ~39
         * de cada 40 frames se descartaban -> los estados (luz/bomba/shore)
         * parpadeaban (se marcaban stale y reaparecian). Confirmado en vivo
         * 2026-06-23 con NE187 desconectado: 703 rx20 fallidos vs 3 parseados.
         *
         * Solucion: escanear el buffer buscando un frame valido en CUALQUIER
         * offset. Con el poll correcto (FF 40 00 00 3F) el NE185 responde con
         * la trama canonica de 20 B (tanques incluidos). Dos casos:
         *   - Caso B (echo presente): el buffer trae [poll 5B][resp 15B] = 20 B
         *     que empieza en FF y pasa checksum_ok directamente.
         *   - Caso A (sin echo): solo llega la resp de 15 B (empieza por el
         *     nibble de tanque, sin cabecera fija). Reconstruimos frame20 =
         *     tx_cmd(5) + resp(15) y validamos con checksum_ok. El checksum
         *     (suma b5..b18) es muy discriminante -> sin falsos positivos.
         * El primero valido gana. (Se elimino el path "nativo F8 E0": era el
         * artefacto de responder al poll erroneo; con el poll bueno no aparece.) */
        for (int k = 0; k + 15 <= n && !ok; k++) {
            if (buf[k] == 0xFF && k + FRAME_LEN <= n && checksum_ok(buf + k)) {
                /* Caso B: trama completa de 20 B (echo del poll + respuesta). */
                memcpy(frame20, buf + k, FRAME_LEN);
                ok = true;
            } else {
                /* Caso A: respuesta de 15 B sin echo -> reconstruir y validar. */
                uint8_t cand[FRAME_LEN];
                memcpy(cand, tx_cmd, 5);
                memcpy(cand + 5, buf + k, 15);
                if (checksum_ok(cand)) {
                    memcpy(frame20, cand, FRAME_LEN);
                    ok = true;
                }
            }
        }

        if (ok) {
            parse_frame(frame20);
            consec_timeouts = 0;

            /* Auto-encendido de cargas: cuando la centralita lleva ya unas
             * cuantas tramas buenas (despierta y estado fiable), si la funcion
             * esta activada encendemos luz int + bomba SI estan apagadas
             * (toggle condicional: nunca las apaga). One-shot por wake. */
            if (s_good_frames < 0xFFFFFFFF) s_good_frames++;
            if (s_autostart_enabled && !s_autostart_done && s_good_frames >= 10) {
                bool lin_off = false, pump_off = false;
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    lin_off  = !s_data.light_in;
                    pump_off = !s_data.pump;
                    xSemaphoreGive(s_mutex);
                }
                char c;
                if (lin_off)  { c = 'i'; xQueueSend(s_press_queue, &c, 0); }
                if (pump_off) { c = 'p'; xQueueSend(s_press_queue, &c, 0); }
                s_autostart_done = true;
                ESP_LOGI(TAG, "autostart: luz_int=%s bomba=%s",
                         lin_off ? "->ON" : "ya-on", pump_off ? "->ON" : "ya-on");
            }
        } else {
            consec_timeouts++;
            if (consec_timeouts == BUS_DEAD_THRESH) {
                /* Bus muerto = centralita apagada. Rearmamos el autostart para
                 * que vuelva a encender cargas cuando despierte de nuevo (util
                 * si el P4 no resetea, p.ej. alimentado por USB). */
                s_autostart_done = false;
                s_good_frames = 0;
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
        /* SHORE ON: IDLE con b[16]=0x31 (230V conectado). El checksum no
         * incluye b[16], asi que b[19] sigue siendo 0x00. Validado 2026-06-22. */
        { "IDLE response, SHORE 230V ON",
          { 0xFF, 0x40, 0x00, 0x00, 0x3F, 0x03, 0x02, 0x00, 0x40, 0x5F,
            0x00, 0xFF, 0x9A, 0xA6, 0xED, 0x00, 0x31, 0x00, 0x00, 0x00 },
          false, false, false, true },
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
    memset(&s_data, 0, sizeof(s_data));

    /* Auto-encendido de cargas al arranque: leer flag persistido. */
    bool autostart = false;
    load_autostart_loads(&autostart);
    s_autostart_enabled = autostart;
    ESP_LOGI(TAG, "autostart cargas al arranque: %s", autostart ? "ON" : "OFF");

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

void ne185_set_autostart(bool enabled)
{
    s_autostart_enabled = enabled;
    save_autostart_loads(enabled);
    /* Rearmar para que, si ya estamos despiertos, se aplique en breve. */
    if (enabled) s_autostart_done = false;
    ESP_LOGW(TAG, ">>> AUTOSTART cargas %s <<<", enabled ? "ON" : "OFF");
}

bool ne185_get_autostart(void)
{
    return s_autostart_enabled;
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
