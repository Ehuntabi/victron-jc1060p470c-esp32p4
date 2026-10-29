/* udp_latido.c - Receptor del latido de la 35cabina (UDP 4243).
 *
 * Por que existe (2-oct-2026): la telemetria va solo de la P4 a la cabina, asi
 * que la P4 no tenia manera de saber si sus paquetes llegaban. En el fallo que
 * perseguimos -- la cabina asociada, el AP de la radio vivo y ni un paquete en
 * ningun sentido -- la P4 se enteraba por la tabla de concesiones del DHCP y
 * tarde. Ahora la cabina manda cada 2 s un paquete minusculo diciendo cuanto
 * lleva SIN recibir telemetria, y la P4 lo usa como fuente de verdad:
 *
 *   - llega el latido            -> la subida (cabina -> P4) funciona;
 *   - el latido dice 0-2 s       -> la bajada (P4 -> cabina) tambien;
 *   - el latido dice 20 s        -> la bajada esta rota: hay que reparar el AP;
 *   - no llega ningun latido     -> la subida esta rota (o la cabina esta apagada).
 *
 * Con eso el vigilante decide en SEGUNDOS en vez de en minutos, y la cabina no
 * tiene que adivinar nada ni ponerse a reconectar por su cuenta.
 *
 * Aqui solo se escucha y se guarda el estado: no se actua. La tarea mantiene una
 * tabla de fuentes (una por cabina), llama a esp_netif_* por paquete para
 * comprobar de que red viene y la comparte con el vigilante del AP y con el
 * portal. ESA TABLA VA CON CERROJO (s_mux_fuentes, tanda 2 de la auditoria del
 * 18-oct-2026): el comentario viejo decia que "son enteros y punteros alineados,
 * y lo peor es leer un dato de una vuelta anterior", y era falso -- ultimo_us es
 * int64_t (dos palabras en RV32) y se puede leer A MEDIAS, no "con el valor de
 * antes". Eso dejaba latido_vivo=false con el enlace sano y el portal tomaba el
 * camino de "cabina muda". Los cuatro lectores y el escritor van bajo el mismo
 * spinlock; dentro solo hay asignaciones (ni logs ni nada que bloquee). */
#include "udp_latido.h"
#include "mini_proto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "watchdog.h"   /* watchdog_heartbeat: la tarea late (ver watchdog.c) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "udp_latido";

/* ── UNA ENTRADA POR CABINA, no un unico estado (auditoria del 13-oct-2026) ──
 *
 * Antes esto eran cuatro variables sueltas: el ULTIMO latido que llegara pisaba
 * al anterior. Con dos pantallas eso es un fallo de verdad: si la 3,5" esta
 * asociada pero muda y la 5" esta sana, el estado bueno de la 5" tapaba a la
 * muda y la P4 no reparaba nada -- la 3,5" se quedaba sin telemetria hasta que
 * alguien la reiniciaba a mano. Ahora hay una entrada por IP de origen. */
#define LATIDO_MAX_FUENTES 4

typedef struct {
    uint32_t ip;                 /* 0 = entrada libre */
    int64_t  ultimo_us;
    int      seg_sin_datos;
    bool     ip_fija;
} latido_fuente_t;

static latido_fuente_t s_fuentes[LATIDO_MAX_FUENTES];

/* CERROJO DE LA TABLA (tanda 2 de la auditoria del 18-oct-2026). La escribe la
 * tarea del latido y la leen la del portal y las de estado: sin cerrojo, un
 * int64_t (dos palabras en RV32) se puede leer A MEDIAS -- no "con el valor de
 * la vuelta anterior", como decia el comentario: un valor arbitrario. Eso deja
 * latido_vivo=false o latiendo=0 con el enlace sano y el portal toma el camino
 * de "cabina muda" (reinicio del AP, o de la placa). Las secciones son de dos
 * lineas: un spinlock vale. */
static portMUX_TYPE s_mux_fuentes = portMUX_INITIALIZER_UNLOCKED;

/* Coge (o crea) la entrada de esa IP. Devuelve NULL si la tabla esta llena: con
 * cuatro huecos y dos cabinas de sobra, pero si un desconocido los llena, lo que
 * NO se hace es pisar a una cabina conocida.
 *
 * SI RECICLA UN HUECO, LA IP VIEJA SALE POR 'reciclada' (tanda 2 de la auditoria
 * del 18-oct-2026): esta funcion se llama con el cerrojo de la tabla cogido y el
 * ESP_LOGI de aqui dentro escribia en el UART con las interrupciones apagadas.
 * El que avisa es el que llama, ya sin cerrojo. */
static latido_fuente_t *fuente_de(uint32_t ip, int64_t ahora_us, uint32_t *reciclada)
{
    latido_fuente_t *libre = NULL;
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == ip) return &s_fuentes[i];
        /* CADUCA: una cabina que cambia de IP (el DHCP le da otra tras
         * reiniciar el AP), una pantalla sustituida o un invitado dejan su
         * hueco ocupado para siempre; con cuatro huecos, la tabla se llenaba y
         * una cabina nueva se quedaba fuera ("tabla llena: lo ignoro") con la
         * deteccion por cabina apagada hasta el siguiente reinicio. Revision del
         * 13-oct-2026. */
        bool caducada = (s_fuentes[i].ip != 0) &&
                        ((ahora_us - s_fuentes[i].ultimo_us) > 60LL * 1000000LL);
        if ((s_fuentes[i].ip == 0 || caducada) && !libre) libre = &s_fuentes[i];
    }
    if (libre && libre->ip != 0) {
        if (reciclada) *reciclada = libre->ip;
        libre->ip = 0;
    }
    return libre;
}

/* La ultima fuente que hablo, que es lo que enseñan la pantalla y los logs. */
static latido_fuente_t *fuente_mas_reciente(void)
{
    latido_fuente_t *mejor = NULL;
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == 0) continue;
        if (!mejor || s_fuentes[i].ultimo_us > mejor->ultimo_us) mejor = &s_fuentes[i];
    }
    return mejor;
}

void udp_latido_estado(udp_latido_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_mux_fuentes);
    latido_fuente_t *f = fuente_mas_reciente();
    if (!f) { portEXIT_CRITICAL(&s_mux_fuentes);
              out->seg_desde_latido = -1; out->seg_sin_datos_mini = -1; return; }
    out->seg_desde_latido = (int)((esp_timer_get_time() - f->ultimo_us) / 1000000);
    out->seg_sin_datos_mini = f->seg_sin_datos;
    out->ip_fija_mini = f->ip_fija;
    out->hay_datos = true;
    out->ip_mini = f->ip;
    portEXIT_CRITICAL(&s_mux_fuentes);
}

/* Cuantas cabinas conocidas han latido en los ultimos 'vivo_s' segundos. */
int udp_latido_fuentes_vivas(int vivo_s)
{
    int64_t ahora = esp_timer_get_time();
    int n = 0;
    portENTER_CRITICAL(&s_mux_fuentes);
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == 0) continue;
        if ((ahora - s_fuentes[i].ultimo_us) / 1000000 <= vivo_s) n++;
    }
    portEXIT_CRITICAL(&s_mux_fuentes);
    return n;
}

bool udp_latido_alguna_callada(int callada_s)
{
    int64_t ahora = esp_timer_get_time();
    bool callada = false;
    portENTER_CRITICAL(&s_mux_fuentes);
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == 0) continue;
        if ((ahora - s_fuentes[i].ultimo_us) / 1000000 > callada_s) { callada = true; break; }
    }
    portEXIT_CRITICAL(&s_mux_fuentes);
    return callada;
}

/* true si ALGUNA cabina que sigue latiendo (esta viva, no se ha ido) dice llevar
 * >= 15 s sin recibir telemetria. Devuelve ademas cuantos segundos son, para el
 * log. Un valor NEGATIVO de seg_sin_datos se trata como mudo: el campo es int16
 * y a las 9 h de silencio da la vuelta (lo satura la cabina desde el
 * 13-oct-2026, pero una cabina con firmware viejo puede mandarlo negativo). */
bool udp_latido_alguno_mudo(int *seg_out)
{
    int64_t ahora = esp_timer_get_time();
    int peor = -1;
    /* TAMBIEN CON CERROJO (tanda 2 de la auditoria del 18-oct-2026): este es el
     * lector que decide "cabina muda" y dispara la reparacion del AP, o sea el
     * que mas duele si lee un ultimo_us a medias. */
    portENTER_CRITICAL(&s_mux_fuentes);
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        latido_fuente_t *f = &s_fuentes[i];
        if (f->ip == 0) continue;
        if ((ahora - f->ultimo_us) / 1000000 >= 15) continue;   /* esta no late: no es fuente */
        int s = f->seg_sin_datos;
        /* -1 es "nunca he recibido nada" (lo que manda una cabina que acaba de
         * arrancar y no ha visto un solo paquete): NO es "llevo mucho sin
         * datos". Tratarlo como mudo hacia que una cabina recien encendida
         * disparara la reparacion del AP en cada arranque (revision del
         * 13-oct-2026). Cualquier OTRO negativo si es mudo: son los valores que
         * daba la vuelta con el int16 antes de saturar. */
        if (s == -1) continue;
        if (s < 0 || s >= 15) {
            if (s < 0) s = 32767;
            if (s > peor) peor = s;
        }
    }
    portEXIT_CRITICAL(&s_mux_fuentes);
    if (peor < 0) return false;
    if (seg_out) *seg_out = peor;
    return true;
}

static void latido_task(void *arg)
{
    (void)arg;

    int sock = -1;
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() fallo: errno=%d (reintento en 5s)", errno);
            watchdog_heartbeat(WD_TASK_UDP_LATIDO);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        struct sockaddr_in dir = {0};
        dir.sin_family      = AF_INET;
        dir.sin_port        = htons(MINI_LATIDO_UDP_PORT);
        dir.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&dir, sizeof(dir)) < 0) {
            ESP_LOGE(TAG, "bind(:%d) fallo: errno=%d (reintento en 5s)",
                     MINI_LATIDO_UDP_PORT, errno);
            close(sock);
            sock = -1;
            watchdog_heartbeat(WD_TASK_UDP_LATIDO);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
    }
    ESP_LOGI(TAG, "Escuchando el latido de la cabina en :%d (sizeof=%u)",
             MINI_LATIDO_UDP_PORT, (unsigned)sizeof(mini_latido_t));

    /* Espera de 2 s: asi la tarea se despierta sola y no se queda colgada si el
     * socket se queda raro tras un reinicio del AP. */
    struct timeval espera = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &espera, sizeof(espera));

    uint8_t buf[64];
    struct sockaddr_in src;
    for (;;) {
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        /* Late en CADA vuelta (llega latido o salta el timeout de 2 s): lo que
         * vigila el watchdog es que la tarea siga viva, no que la cabina hable.
         * Si esta tarea se colgara, la P4 creeria que la cabina se ha muerto y
         * reiniciaria su AP en cascada (ver el comentario en watchdog.h). */
        watchdog_heartbeat(WD_TASK_UDP_LATIDO);
        if (n < 0) continue;                    /* timeout o error: no hay nada que hacer */
        if (n != (int)sizeof(mini_latido_t)) {
            ESP_LOGW(TAG, "latido de tamano raro: %d (esperado %u)",
                     n, (unsigned)sizeof(mini_latido_t));
            continue;
        }
        const mini_latido_t *l = (const mini_latido_t *)buf;
        if (l->magic != MINI_LATIDO_MAGIC) continue;
        /* SU PROPIA VERSION (auditoria del 27-oct-2026, B1): antes se comparaba
         * con MINI_PROTO_VERSION, la del struct grande de telemetria. Un bump de
         * ese struct (que pasa cada pocos meses) dejaba sordos a los latidos de
         * una cabina sin regrabar, y con ellos el aviso de "esta cabina no
         * recibe" y la reparacion del AP a los 15 s. Vale 5, lo mismo que
         * MINI_PROTO_VERSION, asi que una cabina de la v1.46 sigue encajando. */
        if (l->version != MINI_LATIDO_VERSION) {
            ESP_LOGW(TAG, "latido con version %u (espero %u): la cabina lleva otro "
                          "firmware", l->version, MINI_LATIDO_VERSION);
            continue;
        }
        uint32_t esperado = esp_crc32_le(0, buf, sizeof(mini_latido_t) - sizeof(uint32_t));
        if (esperado != l->crc32) {
            ESP_LOGW(TAG, "latido con CRC malo");
            continue;
        }
        /* DE DONDE VIENE (auditoria de seguridad del 13-oct-2026): antes valia
         * cualquier origen, y con un latido falso se podia hacer que la P4
         * reiniciara su AP en bucle -- y hasta la placa --. El CRC32 no
         * autentica nada. Ahora solo se aceptan las IP del propio AP y solo si
         * son de una cabina conocida o hay hueco para conocerla; lo que llegue
         * de la puerta (la propia P4) o de fuera de la subred se tira. */
        uint32_t origen = src.sin_addr.s_addr;
        uint32_t red_ap = 0, mascara_ap = 0;
        {
            esp_netif_ip_info_t ip;
            esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
                red_ap = ip.ip.addr;
                mascara_ap = ip.netmask.addr;   /* la de verdad, no un /24 a mano */
            }
        }
        /* FAIL-CLOSED: si no se puede saber cual es la red del AP, el latido se
         * tira. Antes se saltaba la comprobacion entera (se aceptaba cualquier
         * origen) justo cuando el netif esta raro, que es cuando mas importa. */
        if (red_ap == 0 || mascara_ap == 0) {
            ESP_LOGW(TAG, "no puedo leer la IP del AP: ignoro el latido de %s",
                     inet_ntoa(src.sin_addr));
            continue;
        }
        if ((origen & mascara_ap) != (red_ap & mascara_ap) || origen == red_ap) {
            ESP_LOGW(TAG, "latido de fuera del AP (%s): lo ignoro",
                     inet_ntoa(src.sin_addr));
            continue;
        }
        /* NO se filtra por la franja .200-.231, y esto se probo en el banco el
         * 13-oct-2026: esa franja es SOLO la IP de emergencia que se pone la
         * cabina cuando el DHCP de la P4 no contesta (ver udp_rx.c). Con el DHCP
         * funcionando -- que es lo normal -- la cabina tiene una concesion de
         * .2 a .101, asi que exigir .200+ TIRABA TODOS SUS LATIDOS y la P4 se
         * quedaba sin la unica fuente de verdad de la bajada (el sintoma:
         * "Primer latido de una cabina" no aparecia nunca en el log). Lo que
         * queda es el filtro de subred del AP (que descarta a cualquiera de
         * fuera) mas el tope y la caducidad de la tabla. */
        uint32_t reciclada = 0;
        portENTER_CRITICAL(&s_mux_fuentes);
        latido_fuente_t *f = fuente_de(origen, esp_timer_get_time(), &reciclada);
        if (!f) {
            portEXIT_CRITICAL(&s_mux_fuentes);
            ESP_LOGW(TAG, "latido de %s y la tabla de cabinas esta llena: lo ignoro",
                     inet_ntoa(src.sin_addr));
            continue;
        }
        bool primera = (f->ip == 0);
        f->ip = origen;
        f->ultimo_us = esp_timer_get_time();
        /* El campo es int16 en el protocolo: saturado a lo que quepa. */
        f->seg_sin_datos = (l->seg_sin_datos > 32000) ? 32000 : (int)l->seg_sin_datos;
        f->ip_fija = (l->ip_fija != 0);
        portEXIT_CRITICAL(&s_mux_fuentes);
        if (reciclada) {
            ESP_LOGI(TAG, "hueco de latido reciclado (la cabina %08lx llevaba >60 s callada)",
                     (unsigned long)reciclada);
        }
        if (primera) {
            ESP_LOGI(TAG, "Primer latido de una cabina (%s): lleva %d s sin datos%s",
                     inet_ntoa(src.sin_addr), (int)l->seg_sin_datos,
                     l->ip_fija ? ", con IP fija" : "");
        }
    }
}

void udp_latido_start(void)
{
    /* Pila pequena: solo recibe 12 bytes y escribe cuatro variables. */
    if (xTaskCreate(latido_task, "udp_latido", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no he podido crear la tarea del latido: el vigilante del AP "
                      "no sabra como va el enlace");
    }
}
