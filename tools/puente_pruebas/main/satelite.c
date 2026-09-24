/* satelite.c — El papel de la cabina (35cabina) contra la P4.
 *
 * QUE HACE
 *   1. Se conecta al AP de la P4 como cliente.
 *   2. Escucha el broadcast UDP de telemetria (puerto 4242, mini_proto v5) y
 *      COMPRUEBA el CRC de cada paquete: asi se mide si la P4 los manda todos y
 *      si alguno llega roto (perdida de datos).
 *   3. Hace las mismas peticiones HTTP que hace la cabina o la app: GET
 *      /api/state a 1 Hz, paginas y CSV de vez en cuando, /snapshot (camara) y
 *      POST /api/alarma. OJO: los CSV, /ota y /vigilancia viven en el puerto
 *      8081 (servidor "pesado"), no en el 80; pedirlos al 80 da 404. Del POST /api/viaje solo se prueban cuerpos invalidos
 *      (los buenos crearian un viaje de verdad que se quedaria abierto, y desde
 *      fuera no hay forma de cerrarlo): se comprueba que la P4 los rechaza bien.
 *
 * Todo queda contado y se saca con 'sat informe'.
 */
#include "satelite.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "satelite";

#define PUERTO_UDP 4242
#define TAM_MSG    40

static volatile bool s_activo = false;
static char s_ssid[33] = "", s_clave[65] = "", s_user[33] = "", s_pass[65] = "";
static char s_auth[200] = "";        /* "Basic ...." ya montado */
static char s_ip[32] = "";

/* Contadores */
static uint32_t s_udp_total = 0, s_udp_crc_mal = 0, s_udp_version_mal = 0;
static uint32_t s_http_total = 0, s_http_ok = 0, s_http_4xx = 0, s_http_error = 0;
static int64_t  s_ultimo_udp = 0;
static int32_t  s_soc = -1, s_volt = -1, s_corr = 0;
static uint8_t  s_alarmas = 0;
static uint32_t s_epoch = 0;

/* ── WiFi ───────────────────────────────────────────────────────────────── */
static void eventos_wifi(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    (void)arg; (void)datos;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        printf("SAT conectado a \"%s\"\n", s_ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        printf("SAT desconectado, reintento\n");
        if (s_activo) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)datos;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        printf("SAT IP=%s\n", s_ip);
    }
}

/* ── Telemetria UDP ─────────────────────────────────────────────────────── */
static void tarea_udp(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { printf("SAT ERR no puedo abrir el socket UDP\n"); vTaskDelete(NULL); return; }

    struct sockaddr_in dir = {0};
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = htonl(INADDR_ANY);
    dir.sin_port = htons(PUERTO_UDP);
    if (bind(s, (struct sockaddr *)&dir, sizeof(dir)) < 0) {
        printf("SAT ERR no puedo escuchar en el %d\n", PUERTO_UDP);
        close(s); vTaskDelete(NULL); return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    printf("SAT escuchando la telemetria en el puerto %d\n", PUERTO_UDP);

    uint8_t buf[128];
    while (s_activo) {
        int n = recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
        if (n < TAM_MSG) continue;              /* timeout o paquete corto */
        s_udp_total++;
        s_ultimo_udp = esp_timer_get_time();

        /* El CRC32 cubre los 36 primeros bytes y viaja en los 4 ultimos. */
        uint32_t crc_rx, crc_calc;
        memcpy(&crc_rx, buf + 36, 4);
        crc_calc = esp_crc32_le(0, buf, 36);
        if (crc_rx != crc_calc) s_udp_crc_mal++;
        if (buf[0] != 5) s_udp_version_mal++;

        int16_t soc, volt; int32_t corr;
        memcpy(&soc,  buf + 2, 2);
        memcpy(&volt, buf + 4, 2);
        memcpy(&corr, buf + 6, 4);
        s_soc = soc; s_volt = volt; s_corr = corr;

        uint16_t frigo; uint8_t limpia, grises;
        memcpy(&frigo, buf + 16, 2);
        limpia = buf[20]; grises = buf[21];
        s_alarmas = buf[34];
        memcpy(&s_epoch, buf + 28, 4);

        /* Una linea cada 10 paquetes: si no, inunda la consola. */
        if ((s_udp_total % 10) == 1) {
            printf("UDP %u: soc=%d V=%d I=%ld frigo=%d agua=%u/%u alarmas=0x%02X%s\n",
                   (unsigned)s_udp_total, (int)soc, (int)volt, (long)corr,
                   (int)frigo, limpia, grises,
                   s_alarmas, (crc_rx != crc_calc) ? "  <-- CRC MAL" : "");
        }
    }
    close(s);
    vTaskDelete(NULL);
}

/* ── Peticiones HTTP ────────────────────────────────────────────────────── */
static int pedir(const char *url, esp_http_client_method_t metodo, const char *cuerpo)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000, .method = metodo };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (s_auth[0]) esp_http_client_set_header(c, "Authorization", s_auth);
    if (cuerpo) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, cuerpo, strlen(cuerpo));
    }
    esp_http_client_perform(c);
    int codigo = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    s_http_total++;
    if (codigo >= 200 && codigo < 300) s_http_ok++;
    else if (codigo >= 400 && codigo < 500) s_http_4xx++;
    else if (codigo <= 0) s_http_error++;
    return codigo;
}

static void tarea_http(void *arg)
{
    (void)arg;
    int vuelta = 0;
    while (s_activo) {
        /* Lo que hace la app: /api/state a 1 Hz. */
        pedir("http://192.168.4.1/api/state", HTTP_METHOD_GET, NULL);

        if (vuelta % 10 == 3) {
            int c = pedir("http://192.168.4.1:8081/data/frigo.csv", HTTP_METHOD_GET, NULL);
            if (vuelta % 50 == 3) printf("SAT GET :8081/data/frigo.csv -> %d\n", c);
        }
        if (vuelta % 20 == 7) {
            int c = pedir("http://192.168.4.1/dashboard", HTTP_METHOD_GET, NULL);
            if (vuelta % 60 == 7) printf("SAT GET /dashboard -> %d\n", c);
        }
        if (vuelta % 30 == 11) {
            int c = pedir("http://192.168.4.1/snapshot", HTTP_METHOD_GET, NULL);
            printf("SAT GET /snapshot (camara) -> %d\n", c);
        }
        if (vuelta % 40 == 13) {
            /* Silenciar alarmas: es lo que manda la cabina, y no rompe nada. */
            int c = pedir("http://192.168.4.1/api/alarma", HTTP_METHOD_POST, "{\"alarmas\":15}");
            printf("SAT POST /api/alarma -> %d\n", c);
        }
        if (vuelta % 40 == 29) {
            /* Cuerpo INVALIDO a proposito: la P4 tiene que contestar 400 y no
             * crear ningun viaje (los buenos se quedarian abiertos). */
            int c = pedir("http://192.168.4.1/api/viaje", HTTP_METHOD_POST, "{\"prueba\":1}");
            printf("SAT POST /api/viaje (invalido a proposito) -> %d\n", c);
        }
        vuelta++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

/* ── Arranque y parada ──────────────────────────────────────────────────── */
bool sat_iniciar(const char *ssid, const char *clave, const char *usuario, const char *clave_portal)
{
    if (s_activo) sat_parar();

    snprintf(s_ssid, sizeof(s_ssid), "%.32s", ssid);
    snprintf(s_clave, sizeof(s_clave), "%.64s", clave);
    snprintf(s_user, sizeof(s_user), "%.32s", usuario ? usuario : "");
    snprintf(s_pass, sizeof(s_pass), "%.64s", clave_portal ? clave_portal : "");

    /* Basic Auth ya montado */
    char plano[100];
    snprintf(plano, sizeof(plano), "%s:%s", s_user, s_pass);
    unsigned char b64[128];
    size_t n = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &n,
                              (const unsigned char *)plano, strlen(plano)) == 0) {
        b64[n] = 0;
        snprintf(s_auth, sizeof(s_auth), "Basic %s", (char *)b64);
    } else {
        s_auth[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventos_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, eventos_wifi, NULL));

    wifi_config_t wc = {0};
    size_t n_ssid = strlen(s_ssid);
    if (n_ssid > sizeof(wc.sta.ssid)) n_ssid = sizeof(wc.sta.ssid);
    memcpy(wc.sta.ssid, s_ssid, n_ssid);
    size_t n_pass = strlen(s_clave);
    if (n_pass > sizeof(wc.sta.password)) n_pass = sizeof(wc.sta.password);
    memcpy(wc.sta.password, s_clave, n_pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_udp_total = s_udp_crc_mal = s_udp_version_mal = 0;
    s_http_total = s_http_ok = s_http_4xx = s_http_error = 0;
    s_activo = true;
    xTaskCreate(tarea_udp,  "sat_udp",  4096, NULL, 4, NULL);
    xTaskCreate(tarea_http, "sat_http", 6144, NULL, 4, NULL);
    return true;
}

void sat_parar(void)
{
    s_activo = false;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_wifi_disconnect();
}

bool sat_activo(void) { return s_activo; }

void sat_informe(void)
{
    int64_t ahora = esp_timer_get_time();
    int64_t hace_ms = s_ultimo_udp ? (ahora - s_ultimo_udp) / 1000 : -1;
    printf("SAT %s  IP=%s\n", s_activo ? "ACTIVO" : "parado", s_ip[0] ? s_ip : "-");
    printf("   telemetria UDP: %u paquetes, %u con CRC mal, %u con version rara\n",
           (unsigned)s_udp_total, (unsigned)s_udp_crc_mal, (unsigned)s_udp_version_mal);
    printf("   ultimo paquete hace %lld ms\n", (long long)hace_ms);
    printf("   ultimos valores: soc=%ld V=%ld I=%ld alarmas=0x%02X epoch=%u\n",
           (long)s_soc, (long)s_volt, (long)s_corr, s_alarmas, (unsigned)s_epoch);
    printf("   HTTP: %u peticiones, %u ok, %u rechazadas(4xx), %u sin respuesta\n",
           (unsigned)s_http_total, (unsigned)s_http_ok,
           (unsigned)s_http_4xx, (unsigned)s_http_error);
}
