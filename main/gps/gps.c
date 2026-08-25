/* gps.c — u-blox NEO-M9N por UART2 (NMEA).
 *
 * Solo se ESCUCHA. El modulo suelta sus tramas solo al encenderse y con eso hay
 * de sobra para posicion, satelites y hora. No se le manda nada por UBX: esos
 * comandos cambian entre versiones de firmware del propio modulo y seria una
 * dependencia que se rompe sola.
 *
 * 38400 BAUDIOS, y no es un numero al azar: es el que trae de fabrica la serie
 * M9. Los M8 de antes venian a 9600, asi que probar a 9600 y no ver nada es el
 * error tipico con estos modulos.
 *
 * SE COMPRUEBA EL CHECKSUM de cada trama. No es celo: si la velocidad no fuera
 * la correcta, o el cable cogiera ruido, llegarian lineas que PARECEN NMEA con
 * numeros creibles dentro. Una posicion inventada es peor que no tener
 * posicion, porque nadie la cuestiona.
 *
 * Pines (ver docs/pinout_guition_jc1060p470c_i.pdf): UART2 estaba libre --
 * UART0 es la consola y UART1 el NE185 por RS-485. GPIO 3 recibe del GPS y
 * GPIO 2 va hacia el (solo haria falta para configurarlo, hoy no se usa, pero
 * dejarlo asignado evita que otro periferico lo ocupe sin darse cuenta).
 */
#include "gps/gps.h"
#include "rtc_rx8025t.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "gps";

#define GPS_UART_NUM   UART_NUM_2
#define GPS_UART_TX    GPIO_NUM_2
#define GPS_UART_RX    GPIO_NUM_3
#define GPS_BAUD       38400
#define GPS_RX_BUF     2048
#define LINEA_MAX      128

/* Sin trama nueva en este tiempo, se considera que el modulo no esta. Las
 * manda una vez por segundo, asi que 5 s es holgado. */
#define SIN_DATO_S     5

/* Cada cuanto se vuelve a poner el reloj en hora. El GPS es mucho mas exacto
 * que el RTC, pero escribir en el RTC cada segundo no aporta nada y le da
 * trabajo al bus I2C que comparte con la pantalla tactil. */
#define RESYNC_S       (6 * 3600)

static SemaphoreHandle_t s_mtx;
static gps_data_t        s_d;
static int64_t           s_ultimo_us;
static uint32_t          s_sincs;

/* Anillo con las ultimas tramas, para la pantalla de diagnostico. */
static char s_crudo[GPS_CRUDO_N][LINEA_MAX];
static int  s_crudo_next;

/* ── Utilidades NMEA ──────────────────────────────────────────────────────── */

/* XOR de todo lo que hay entre '$' y '*', comparado con los dos digitos hex
 * que siguen. Devuelve false si la linea no cuadra. */
static bool checksum_ok(const char *l)
{
    if (*l != '$') return false;
    uint8_t suma = 0;
    const char *p = l + 1;
    while (*p && *p != '*') suma ^= (uint8_t)*p++;
    if (*p != '*') return false;                 /* sin checksum: se descarta */
    unsigned esperado;
    if (sscanf(p + 1, "%2x", &esperado) != 1) return false;
    return suma == (uint8_t)esperado;
}

/* Parte la trama por comas. Machaca el buffer. Devuelve cuantos campos hay. */
static int trocear(char *l, char **campo, int max)
{
    int n = 0;
    campo[n++] = l;
    for (char *p = l; *p && n < max; p++) {
        if (*p == ',') { *p = 0; campo[n++] = p + 1; }
        else if (*p == '*') { *p = 0; break; }
    }
    return n;
}

/* NMEA da la latitud como ddmm.mmmm y la longitud como dddmm.mmmm: grados y
 * minutos PEGADOS, no grados decimales. Hay que separarlos. */
static double a_grados(const char *v, const char *hemi)
{
    if (!v || !*v) return 0.0;
    double crudo = atof(v);
    int grados = (int)(crudo / 100.0);
    double minutos = crudo - grados * 100.0;
    double d = grados + minutos / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

/* Dias desde 1970 sin usar mktime/timegm: newlib no trae timegm y mktime
 * aplicaria el huso local, que aqui NO se quiere -- el GPS da UTC. Algoritmo
 * days_from_civil de Howard Hinnant, el mismo que ya usa net/udp_tx.c. */
static long dias_desde_1970(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + (long)doe - 719468;
}

/* ── Puesta en hora ───────────────────────────────────────────────────────── */

/* hhmmss.ss (UTC) + ddmmyy -> epoch, y de ahi al reloj del sistema y al RTC.
 *
 * Ojo con los dos relojes: el del sistema va en UTC y el chip RX8025T guarda
 * hora LOCAL (asi lo lee main.c al arrancar). Poner UTC en el RTC correria la
 * hora dos horas en verano sin que nada avisara. */
static void poner_en_hora(const char *hhmmss, const char *ddmmyy)
{
    if (!hhmmss || strlen(hhmmss) < 6 || !ddmmyy || strlen(ddmmyy) < 6) return;

    int hh = (hhmmss[0]-'0')*10 + (hhmmss[1]-'0');
    int mi = (hhmmss[2]-'0')*10 + (hhmmss[3]-'0');
    int ss = (hhmmss[4]-'0')*10 + (hhmmss[5]-'0');
    int dd = (ddmmyy[0]-'0')*10 + (ddmmyy[1]-'0');
    int mo = (ddmmyy[2]-'0')*10 + (ddmmyy[3]-'0');
    int yy = (ddmmyy[4]-'0')*10 + (ddmmyy[5]-'0');
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31) return;

    time_t epoch = (time_t)dias_desde_1970(2000 + yy, (unsigned)mo, (unsigned)dd) * 86400
                 + hh * 3600 + mi * 60 + ss;
    if (epoch < 1700000000L) return;          /* fecha absurda: no me la creo */

    /* Guardar lo que dice el GPS, para poder enseñarlo. Va aqui y no en el
     * parseo porque aqui ya se ha validado que la fecha tiene sentido. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_d.fecha, sizeof(s_d.fecha), "%02d-%02d-%04d", dd, mo, 2000 + yy);
    snprintf(s_d.hora,  sizeof(s_d.hora),  "%02d:%02d:%02d", hh, mi, ss);
    xSemaphoreGive(s_mtx);

    /* Solo si hace falta: la primera vez, o cada RESYNC_S, o si el reloj se ha
     * ido mas de 2 s. Reescribir el RTC cada segundo no aporta nada. */
    static int64_t ultima_sinc_us;
    int64_t ahora_us = esp_timer_get_time();
    time_t sistema = time(NULL);
    long desvio = (long)(epoch - sistema); if (desvio < 0) desvio = -desvio;

    bool primera = (ultima_sinc_us == 0);
    bool toca    = (ahora_us - ultima_sinc_us) > (int64_t)RESYNC_S * 1000000LL;
    if (!primera && !toca && desvio <= 2) return;

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    struct tm tm_local;
    localtime_r(&epoch, &tm_local);           /* TZ ya puesta en main.c */
    rtc_set_time(&tm_local);

    ultima_sinc_us = ahora_us;
    s_sincs++;
    ESP_LOGI(TAG, "reloj puesto en hora con el GPS (desvio era %ld s): "
                  "%04d-%02d-%02d %02d:%02d:%02d local", desvio,
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
}

/* ── Tramas ───────────────────────────────────────────────────────────────── */

/* El NEO-M9N ve varias constelaciones a la vez, asi que la cabecera suele ser
 * $GN... y no $GP.... Se mira solo el tipo (las tres ultimas letras) para que
 * valga con cualquier combinacion. */
static void procesar(char *linea)
{
    if (!checksum_ok(linea)) return;

    char *c[24];
    char tipo[4] = {0};
    memcpy(tipo, linea + 3, 3);
    int n = trocear(linea, c, 24);

    if (!strcmp(tipo, "GGA") && n >= 10) {
        int calidad = atoi(c[6]);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_d.hay_fix    = (calidad > 0);
        s_d.satelites  = (uint8_t)atoi(c[7]);
        if (s_d.hay_fix) {
            s_d.lat       = a_grados(c[2], c[3]);
            s_d.lon       = a_grados(c[4], c[5]);
            s_d.altitud_m = (float)atof(c[9]);
        }
        xSemaphoreGive(s_mtx);
    } else if (!strcmp(tipo, "GSV") && n >= 4) {
        /* GSV: satelites a la vista, de cuatro en cuatro por trama. Cada bloque
         * son 4 campos (prn, elevacion, azimut, C/N0) y el C/N0 es el ultimo.
         *
         * Vienen VARIAS tramas por constelacion (GP, GL, GA, GB) y varias por
         * cada una. Se acumula en un temporal y solo se publica al cerrar la
         * ronda -- si se publicara trama a trama, el numero bailaria sin parar.
         * La ronda se cierra cuando llega la ultima trama del grupo (campo 2 ==
         * campo 1) de la ULTIMA constelacion; como no se sabe cual es, se cierra
         * por tiempo: 1,2 s sin GSV = ronda terminada. */
        static uint32_t suma = 0, cuantos = 0, mejor = 0;
        static int64_t ultima_gsv = 0;
        int64_t ahora = esp_timer_get_time();

        if (ultima_gsv && (ahora - ultima_gsv) > 1200000LL) {
            /* Ronda anterior cerrada: publicar y empezar de cero. */
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_d.snr_mejor   = (uint8_t)mejor;
            s_d.snr_medio   = cuantos ? (uint8_t)(suma / cuantos) : 0;
            s_d.snr_cuantos = (uint8_t)(cuantos > 255 ? 255 : cuantos);
            xSemaphoreGive(s_mtx);
            suma = cuantos = mejor = 0;
        }
        ultima_gsv = ahora;

        for (int i = 7; i < n; i += 4) {          /* 7 = C/N0 del primer bloque */
            if (!c[i] || !*c[i]) continue;        /* vacio = satelite sin medir */
            int v = atoi(c[i]);
            if (v <= 0 || v > 99) continue;
            suma += (uint32_t)v; cuantos++;
            if ((uint32_t)v > mejor) mejor = (uint32_t)v;
        }
    } else if (!strcmp(tipo, "RMC") && n >= 10) {
        /* Campo 2 = 'A' valido, 'V' aviso. La hora solo se cree si es 'A':
         * el modulo da hora aproximada desde su reloj interno antes de tener
         * fix, y ponerla seria empeorar la que ya tiene el RTC. */
        if (c[2] && *c[2] == 'A') poner_en_hora(c[1], c[9]);
    }
}

/* Cada cuanto se cuenta como va la cosa MIENTRAS NO HAY POSICION. Diez
 * segundos: lo bastante seguido para enchufar el cable y ver el resultado sin
 * esperar, y se calla solo en cuanto hay fix. */
#define GPS_INFORME_US  (10 * 1000000LL)

static void gps_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    char linea[LINEA_MAX];
    size_t largo = 0;

    /* Sin esto, la unica forma de saber si el GPS esta bien conectado era salir
     * a la calle y esperar a que fijara: si no llegaba nada, el programa no
     * decia ni mu. Ahora se ve al momento si el modulo habla, aunque no vea un
     * satelite -- que es lo normal bajo techo: el NEO-M9N manda sus tramas
     * igual, con los campos vacios. */
    uint32_t tramas = 0, tramas_antes = 0;
    int64_t  informe_us = 0;

    while (1) {
        int leidos = uart_read_bytes(GPS_UART_NUM, buf, sizeof(buf),
                                     pdMS_TO_TICKS(200));
        int64_t ahora_us = esp_timer_get_time();

        for (int i = 0; i < leidos; i++) {
            char ch = (char)buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                linea[largo] = 0;
                if (largo > 6) {
                    /* Se guarda en el anillo ANTES de validar: para diagnosticar
                     * lo que hace falta ver es justo la basura, no lo que ya
                     * sabemos que esta bien. */
                    xSemaphoreTake(s_mtx, portMAX_DELAY);
                    snprintf(s_crudo[s_crudo_next], LINEA_MAX, "%s", linea);
                    s_crudo_next = (s_crudo_next + 1) % GPS_CRUDO_N;
                    s_ultimo_us = ahora_us;
                    s_d.hay_datos = true;
                    xSemaphoreGive(s_mtx);

                    tramas++;
                    char copia[LINEA_MAX];
                    memcpy(copia, linea, largo + 1);
                    procesar(copia);
                }
                largo = 0;
            } else if (largo < LINEA_MAX - 1) {
                linea[largo++] = ch;
            } else {
                largo = 0;      /* linea imposible: se tira y a esperar la '\n' */
            }
        }

        /* Caducidad: si deja de hablar, el estado tiene que reflejarlo. Sin
         * esto la pantalla se quedaria con el ultimo fix bueno para siempre y
         * pareceria que hay posicion con el cable desenchufado. */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_ultimo_us && (ahora_us - s_ultimo_us) > (int64_t)SIN_DATO_S * 1000000LL) {
            s_d.hay_datos = false;
            s_d.hay_fix   = false;
            s_d.satelites = 0;
            s_d.snr_mejor = s_d.snr_medio = s_d.snr_cuantos = 0;
        }
        bool fix  = s_d.hay_fix;
        uint8_t sats = s_d.satelites;
        /* La ultima trama tal cual llego, para el parte de abajo. Se copia
         * dentro del cerrojo: el anillo lo escribe este mismo bucle, pero lo
         * lee tambien la pantalla. */
        char ultima[LINEA_MAX];
        ultima[0] = 0;
        if (tramas) {
            int prev = (s_crudo_next - 1 + GPS_CRUDO_N) % GPS_CRUDO_N;
            snprintf(ultima, sizeof(ultima), "%s", s_crudo[prev]);
        }
        xSemaphoreGive(s_mtx);

        /* El parte, mientras no haya posicion. */
        if (!fix && ahora_us - informe_us > GPS_INFORME_US) {
            informe_us = ahora_us;
            if (tramas == tramas_antes) {
                ESP_LOGW(TAG, "NO llega NADA por el UART%d. Mira: TX del GPS al "
                              "GPIO%d y RX al GPIO%d (van CRUZADOS), alimentacion "
                              "de 3,3 V, y que el modulo sea de 38400 baudios",
                         GPS_UART_NUM, GPS_UART_RX, GPS_UART_TX);
            } else {
                /* Con una trama de muestra: es lo que de verdad se mira cuando
                 * algo no cuadra -- si llega basura, se ve aqui. */
                ESP_LOGI(TAG, "hablando: %lu tramas, %u satelites a la vista, "
                              "todavia SIN posicion (bajo techo es lo normal). "
                              "Ultima: %s",
                         (unsigned long)(tramas - tramas_antes), sats, ultima);
            }
            tramas_antes = tramas;
        }
    }
}

/* ── API ──────────────────────────────────────────────────────────────────── */

void gps_get(gps_data_t *out)
{
    if (!out) return;
    /* La UI se construye ANTES que gps_init() (fase 1 contra fase 2 del
     * arranque) y su temporizador de 1 s puede preguntar cuando el cerrojo
     * todavia no existe. Hoy no llega a pasar, pero depender de que la SD tarde
     * lo justo es una bomba de relojeria: xSemaphoreTake(NULL) revienta. */
    if (!s_mtx) { *out = (gps_data_t){0}; return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_d;
    out->segundos_sin_dato = s_ultimo_us
        ? (uint32_t)((esp_timer_get_time() - s_ultimo_us) / 1000000)
        : 0xFFFFFFFF;
    xSemaphoreGive(s_mtx);
}

void gps_crudo_get(int i, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = 0;
    if (i < 0 || i >= GPS_CRUDO_N || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* s_crudo_next apunta al hueco que se escribira; el mas antiguo esta ahi. */
    snprintf(out, n, "%s", s_crudo[(s_crudo_next + i) % GPS_CRUDO_N]);
    xSemaphoreGive(s_mtx);
}

uint32_t gps_sincronizaciones(void) { return s_sincs; }

void gps_init(void)
{
    s_mtx = xSemaphoreCreateMutex();

    const uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(GPS_UART_NUM, GPS_RX_BUF, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(GPS_UART_NUM, &cfg) != ESP_OK ||
        uart_set_pin(GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo abrir el UART del GPS");
        return;
    }

    xTaskCreate(gps_task, "gps", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "NEO-M9N en UART%d (RX=GPIO%d TX=GPIO%d) a %d baudios",
             GPS_UART_NUM, GPS_UART_RX, GPS_UART_TX, GPS_BAUD);
}
