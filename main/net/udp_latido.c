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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "udp_latido";

static volatile int64_t s_ultimo_latido_us = 0;
static volatile int     s_seg_sin_datos_mini = -1;
static volatile bool    s_ip_fija_mini = false;
static volatile uint32_t s_ip_mini = 0;      /* de donde vino el ultimo latido */

void udp_latido_estado(udp_latido_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    int64_t t = s_ultimo_latido_us;
    out->seg_desde_latido = (t == 0) ? -1 : (int)((esp_timer_get_time() - t) / 1000000);
    out->seg_sin_datos_mini = s_seg_sin_datos_mini;
    out->ip_fija_mini = s_ip_fija_mini;
    out->hay_datos = (t != 0);
    out->ip_mini = s_ip_mini;
}

static void latido_task(void *arg)
{
    (void)arg;

    int sock = -1;
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() fallo: errno=%d (reintento en 5s)", errno);
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
        bool primera = (s_ultimo_latido_us == 0);
        s_ultimo_latido_us = esp_timer_get_time();
        s_seg_sin_datos_mini = l->seg_sin_datos;
        s_ip_fija_mini = (l->ip_fija != 0);
        s_ip_mini = src.sin_addr.s_addr;
        if (primera) {
            ESP_LOGI(TAG, "Primer latido de la cabina (%s): lleva %d s sin datos%s",
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
