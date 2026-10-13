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
 * Aqui solo se escucha y se guarda el estado: no se actua. Delicateza: la tarea
 * solo escribe cuatro variables volatiles y no llama a ninguna API del Wi-Fi. */
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
#include "esp_timer.h"
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

/* Coge (o crea) la entrada de esa IP. Devuelve NULL si la tabla esta llena: con
 * cuatro huecos y dos cabinas de sobra, pero si un desconocido los llena, lo que
 * NO se hace es pisar a una cabina conocida. */
static latido_fuente_t *fuente_de(uint32_t ip)
{
    latido_fuente_t *libre = NULL;
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == ip) return &s_fuentes[i];
        if (s_fuentes[i].ip == 0 && !libre) libre = &s_fuentes[i];
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
    latido_fuente_t *f = fuente_mas_reciente();
    if (!f) { out->seg_desde_latido = -1; out->seg_sin_datos_mini = -1; return; }
    out->seg_desde_latido = (int)((esp_timer_get_time() - f->ultimo_us) / 1000000);
    out->seg_sin_datos_mini = f->seg_sin_datos;
    out->ip_fija_mini = f->ip_fija;
    out->hay_datos = true;
    out->ip_mini = f->ip;
}

/* Cuantas cabinas conocidas han latido en los ultimos 'vivo_s' segundos. */
int udp_latido_fuentes_vivas(int vivo_s)
{
    int64_t ahora = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        if (s_fuentes[i].ip == 0) continue;
        if ((ahora - s_fuentes[i].ultimo_us) / 1000000 <= vivo_s) n++;
    }
    return n;
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
    for (int i = 0; i < LATIDO_MAX_FUENTES; i++) {
        latido_fuente_t *f = &s_fuentes[i];
        if (f->ip == 0) continue;
        if ((ahora - f->ultimo_us) / 1000000 >= 15) continue;   /* esta no late: no es fuente */
        int s = f->seg_sin_datos;
        if (s < 0 || s >= 15) {
            if (s < 0) s = 32767;                                /* negativo = mudo */
            if (s > peor) peor = s;
        }
    }
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
        if (l->version != MINI_PROTO_VERSION) {
            ESP_LOGW(TAG, "latido con version %u (espero %u): la cabina lleva otro "
                          "firmware", l->version, MINI_PROTO_VERSION);
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
        uint32_t red_ap = 0;
        {
            esp_netif_ip_info_t ip;
            esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) red_ap = ip.ip.addr;
        }
        if (red_ap != 0) {
            uint32_t mascara = htonl(0xFFFFFF00u);
            if ((origen & mascara) != (red_ap & mascara) || origen == red_ap) {
                ESP_LOGW(TAG, "latido de fuera del AP (%s): lo ignoro",
                         inet_ntoa(src.sin_addr));
                continue;
            }
        }
        latido_fuente_t *f = fuente_de(origen);
        if (!f) {
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
