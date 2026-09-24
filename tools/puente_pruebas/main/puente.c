/* puente.c — Puente de pruebas entre el PC y la P4 (WiFi y BLE).
 *
 * HERRAMIENTA DE BANCO. Se graba en una placa ESP32 APARTE, nunca en la P4.
 * El PC le manda ordenes por el USB y la placa hace de:
 *
 *   - Victron (BLE): anuncia una trama de monitor de bateria con el formato
 *     real (empaquetado a bits) cifrada con la clave del equipo simulado, para
 *     comprobar que la P4 la recibe y la descifra.
 *   - Satelite (WiFi): se conecta al AP de la P4, escanea, pide el portal por
 *     HTTP y manda datagramas UDP. Asi se prueban las dos conexiones sin sacar
 *     el vehiculo del garaje y sin pelearse con permisos del PC.
 *
 * Ordenes (escribe 'help'):
 *   status
 *   blescan on|off
 *   blevictron <mac> <clave32hex> <V_centi> <I_milli> <soc_deci> <ttg_min>
 *   bleadv <mac> <hex...>      (datos de fabricante crudos)
 *   bleoff
 *   wifiscan
 *   wifista <ssid> <clave>
 *   wifioff
 *   wifiip
 *   httpget <url>
 *   udp <ip> <puerto> <hex...> [cada_ms] [veces]
 *
 * Cada orden termina en una linea "OK" o "ERR ...", para que el programa del PC
 * sepa cuando ha acabado.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"

#include "simulador.h"
#include "satelite.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "puente";

/* ── Estado ─────────────────────────────────────────────────────────────── */
static bool s_ble_listo = false;
static bool s_escaneando = false;
static bool s_anunciando = false;
static char s_ip[32] = "";
static char s_ssid[33] = "";

/* ── Utilidades ─────────────────────────────────────────────────────────── */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "AA:BB:CC:DD:EE:FF" -> 6 bytes en orden NimBLE (addr[0] = ultimo octeto) */
static bool parse_mac(const char *txt, uint8_t out[6])
{
    int v[6], n = 0;
    for (const char *p = txt; *p && n < 6; p++) {
        int a = hexval(p[0]), b = hexval(p[1]);
        if (a < 0 || b < 0) return false;
        v[n++] = a * 16 + b;
        p++;
        if (*++p == '\0') break;
    }
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[5 - i];
    return true;
}

/* "AABBCC" -> bytes; devuelve cuantos o -1 */
static int parse_hex(const char *txt, uint8_t *out, int max)
{
    int n = 0;
    while (*txt && n < max) {
        while (*txt == ' ' || *txt == ':' || *txt == '-') txt++;
        if (!*txt) break;
        int a = hexval(txt[0]);
        int b = txt[1] ? hexval(txt[1]) : -1;
        if (a < 0 || b < 0) return -1;
        out[n++] = (uint8_t)(a * 16 + b);
        txt += 2;
    }
    return n;
}

/* ── BLE: anunciar ──────────────────────────────────────────────────────── */
static uint8_t s_mfg[64];
static int s_mfg_len = 0;
static uint8_t s_addr[6];
static char s_addr_txt[18] = "";

static void anunciar(void)
{
    if (!s_ble_listo || s_mfg_len <= 0) return;

    ble_gap_adv_stop();
    if (ble_hs_id_set_rnd(s_addr) != 0) {
        printf("ERR no puedo ponerme la direccion %s\n", s_addr_txt);
        return;
    }
    struct ble_hs_adv_fields campos = {0};
    campos.mfg_data = s_mfg;
    campos.mfg_data_len = (uint8_t)s_mfg_len;
    int rc = ble_gap_adv_set_fields(&campos);
    if (rc != 0) { printf("ERR adv_set_fields rc=%d\n", rc); return; }

    struct ble_gap_adv_params prm = {0};
    prm.conn_mode = BLE_GAP_CONN_MODE_NON;
    prm.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &prm, NULL, NULL);
    if (rc != 0) { printf("ERR adv_start rc=%d\n", rc); return; }
    s_anunciando = true;
}

/* Construye la trama de un monitor de bateria Victron (SmartShunt) y la cifra.
 * Formato:  E1 02 (Victron) | 10 (producto) | largo | producto(2) |
 *           tipo de registro (02) | nonce(2) | clave[0] | 15 bytes cifrados
 * Los 15 bytes en claro son: TTG(2) V(2) alarma(2) aux(2) y luego, EMPAQUETADO
 * A BITS: 2 de entrada auxiliar + 22 de corriente + 20 de consumo + 10 de SOC. */
static int construir_victron(const char *clave_hex, int v_centi, int i_milli,
                             int soc_deci, int ttg_min, uint16_t nonce)
{
    uint8_t clave[16];
    if (parse_hex(clave_hex, clave, 16) != 16) return -1;

    uint8_t claro[15];
    claro[0] = ttg_min & 0xFF; claro[1] = (ttg_min >> 8) & 0xFF;
    claro[2] = v_centi & 0xFF; claro[3] = (v_centi >> 8) & 0xFF;
    claro[4] = 0; claro[5] = 0;                 /* alarma */
    claro[6] = 0; claro[7] = 0;                 /* auxiliar */
    uint64_t cola = 0;                          /* bits 0-1: entrada auxiliar = 0 */
    cola |= ((uint64_t)(i_milli & 0x3FFFFF)) << 2;    /* bits 2-23: corriente (22) */
    /* bits 24-43: consumo (20) -> no lo simulamos, va a 0 */
    cola |= ((uint64_t)(soc_deci & 0x3FF)) << 44;     /* bits 44-53: SOC (10) */
    for (int i = 0; i < 7; i++) claro[8 + i] = (uint8_t)((cola >> (8 * i)) & 0xFF);

    uint8_t cifrado[16] = {0};
    uint8_t ctr[16] = { (uint8_t)(nonce & 0xFF), (uint8_t)(nonce >> 8) };
    uint8_t stream[16] = {0};
    size_t offset = 0;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, clave, 128) != 0) { mbedtls_aes_free(&aes); return -1; }
    if (mbedtls_aes_crypt_ctr(&aes, sizeof(claro), &offset, ctr, stream, claro, cifrado) != 0) {
        mbedtls_aes_free(&aes); return -1;
    }
    mbedtls_aes_free(&aes);

    s_mfg[0] = 0xE1; s_mfg[1] = 0x02;          /* Victron */
    s_mfg[2] = 0x10;                            /* registro de producto */
    s_mfg[3] = (uint8_t)(1 + 2 + 1 + 2 + 1 + sizeof(claro));  /* largo */
    s_mfg[4] = 0x89; s_mfg[5] = 0xA3;          /* SmartShunt 500A/50mV */
    s_mfg[6] = 0x02;                            /* monitor de bateria */
    s_mfg[7] = (uint8_t)(nonce & 0xFF);
    s_mfg[8] = (uint8_t)(nonce >> 8);
    s_mfg[9] = clave[0];                        /* lo comprueba la P4 */
    memcpy(&s_mfg[10], cifrado, sizeof(claro));
    s_mfg_len = 10 + sizeof(claro);
    return 0;
}

/* ── BLE: escanear ──────────────────────────────────────────────────────── */
static int scan_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields f = {0};
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) return 0;

    const uint8_t *a = event->disc.addr.val;
    uint16_t vid = (f.mfg_data_len >= 2 && f.mfg_data)
        ? (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8)) : 0xFFFF;
    printf("ADV %02X:%02X:%02X:%02X:%02X:%02X rssi=%d tipo=%u mfg_len=%u vid=0x%04X \"%s\"\n",
           a[5], a[4], a[3], a[2], a[1], a[0], (int)event->disc.rssi,
           (unsigned)event->disc.addr.type, (unsigned)f.mfg_data_len, vid,
           f.name && f.name_len ? (const char *)f.name : "");
    return 0;
}

/* ── WiFi ───────────────────────────────────────────────────────────────── */
static void wifi_eventos(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        printf("WIFI conectado a \"%s\"\n", s_ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        printf("WIFI desconectado\n");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        printf("WIFI IP=%s\n", s_ip);
    }
}

static void wifi_arrancar(void)
{
    static bool iniciado = false;
    if (iniciado) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_eventos, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_eventos, NULL));
    iniciado = true;
}

/* ── Ordenes ────────────────────────────────────────────────────────────── */
/* La IP se lee SIEMPRE de la netif, no de una copia cacheada: el camino
 * 'wifista' y el camino 'sat' son dos formas de asociarse a la MISMA estacion,
 * y tener dos copias hacia que 'wifiip' dijera "(sin IP)" con el 'sat' ya
 * conectado (la copia de puente.c solo la rellenaba 'wifista'). */
static const char *ip_sta_actual(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
            return s_ip;
        }
    }
    return s_ip[0] ? s_ip : NULL;   /* ultimo valor conocido, si no hay netif */
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *ip  = ip_sta_actual();
    const char *ssid = s_ssid[0] ? s_ssid : sat_ssid();
    printf("BLE  : %s%s%s\n", s_ble_listo ? "listo" : "no listo",
           s_anunciando ? ", emitiendo" : "", s_escaneando ? ", escaneando" : "");
    if (s_anunciando) printf("BLE  : como %s (%d bytes de fabricante)\n", s_addr_txt, s_mfg_len);
    printf("WIFI : %s%s%s\n", ssid[0] ? ssid : "(sin configurar)",
           ip ? " IP=" : "", ip ? ip : "");
    printf("OK\n");
    return 0;
}

static int cmd_blescan(int argc, char **argv)
{
    if (argc < 2) { printf("ERR uso: blescan on|off\n"); return 1; }
    if (!s_ble_listo) { printf("ERR el BLE no esta listo todavia\n"); return 1; }

    if (!strcmp(argv[1], "off")) {
        ble_gap_disc_cancel();
        s_escaneando = false;
        printf("OK escaneo parado\n");
        return 0;
    }
    struct ble_gap_disc_params prm = {0};
    prm.passive = 1;
    prm.filter_duplicates = 1;   /* no repetir el mismo anuncio */
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &prm, scan_cb, NULL);
    if (rc != 0) { printf("ERR ble_gap_disc rc=%d\n", rc); return 1; }
    s_escaneando = true;
    printf("OK escaneando\n");
    return 0;
}

static int cmd_blevictron(int argc, char **argv)
{
    if (argc < 7) {
        printf("ERR uso: blevictron <mac> <clave32hex> <V_centi> <I_milli> <soc_deci> <ttg_min>\n");
        printf("ERR ejemplo: blevictron F9:EB:87:DB:E7:59 00112233445566778899AABBCCDDEEFF 1234 -5678 777 321\n");
        return 1;
    }
    if (!parse_mac(argv[1], s_addr)) { printf("ERR MAC mal formada\n"); return 1; }
    snprintf(s_addr_txt, sizeof(s_addr_txt), "%s", argv[1]);

    static uint16_t nonce = 1;
    if (construir_victron(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
                          atoi(argv[6]), nonce++) != 0) {
        printf("ERR no he podido construir la trama (clave mal?)\n");
        return 1;
    }
    anunciar();
    if (!s_anunciando) return 1;
    printf("OK emitiendo como %s: %d cV, %d mA, %d d%% , %d min\n",
           s_addr_txt, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
    return 0;
}

static int cmd_bleadv(int argc, char **argv)
{
    if (argc < 3) { printf("ERR uso: bleadv <mac> <hex...>\n"); return 1; }
    if (!parse_mac(argv[1], s_addr)) { printf("ERR MAC mal formada\n"); return 1; }
    snprintf(s_addr_txt, sizeof(s_addr_txt), "%s", argv[1]);

    uint8_t datos[40];
    int n = 0;
    for (int i = 2; i < argc; i++) {
        int m = parse_hex(argv[i], datos + n, (int)sizeof(datos) - n);
        if (m < 0) { printf("ERR hex mal formado en '%s'\n", argv[i]); return 1; }
        n += m;
    }
    if (n < 2) { printf("ERR hacen falta al menos 2 bytes (el fabricante)\n"); return 1; }
    memcpy(s_mfg, datos, n); s_mfg_len = n;
    anunciar();
    if (!s_anunciando) return 1;
    printf("OK emitiendo %d bytes de fabricante como %s\n", n, s_addr_txt);
    return 0;
}

static int cmd_bleoff(int argc, char **argv)
{
    (void)argc; (void)argv;
    ble_gap_adv_stop();
    s_anunciando = false;
    printf("OK emision parada\n");
    return 0;
}

static int cmd_wifiscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    wifi_arrancar();
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { printf("ERR scan fallo\n"); return 1; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!aps) { printf("ERR sin memoria\n"); return 1; }
    if (esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            printf("AP rssi=%d canal=%u cifrado=%d ssid=\"%s\"\n", aps[i].rssi,
                   aps[i].primary, (int)aps[i].authmode, (const char *)aps[i].ssid);
        }
    }
    free(aps);
    printf("OK %u redes\n", (unsigned)n);
    return 0;
}

static int cmd_wifista(int argc, char **argv)
{
    if (argc < 3) { printf("ERR uso: wifista <ssid> <clave>\n"); return 1; }
    wifi_arrancar();
    snprintf(s_ssid, sizeof(s_ssid), "%s", argv[1]);
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", argv[1]);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", argv[2]);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();
    printf("OK conectando a \"%s\" (mira las lineas WIFI de arriba)\n", argv[1]);
    return 0;
}

static int cmd_wifioff(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_wifi_disconnect();
    printf("OK desconectando\n");
    return 0;
}

static int cmd_wifiip(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *ip = ip_sta_actual();
    printf("IP %s\n", ip ? ip : "(sin IP)");
    printf("OK\n");
    return 0;
}

static esp_err_t http_evento(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA) {
        int n = ev->data_len > 200 ? 200 : ev->data_len;
        printf("BODY %.*s\n", n, (char *)ev->data);
    }
    return ESP_OK;
}

static int cmd_httpget(int argc, char **argv)
{
    if (argc < 2) { printf("ERR uso: httpget <url>\n"); return 1; }
    esp_http_client_config_t cfg = { .url = argv[1], .timeout_ms = 8000,
                                     .event_handler = http_evento };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { printf("ERR no puedo crear el cliente HTTP\n"); return 1; }
    /* httpget <url> [usuario] [clave]  -> con Basic Auth (el portal lo pide) */
    if (argc >= 4) {
        char plano[100], cab[200];
        unsigned char b64[128];
        size_t n = 0;
        snprintf(plano, sizeof(plano), "%.32s:%.64s", argv[2], argv[3]);
        if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &n,
                                  (const unsigned char *)plano, strlen(plano)) == 0) {
            b64[n] = 0;
            snprintf(cab, sizeof(cab), "Basic %s", (char *)b64);
            esp_http_client_set_header(c, "Authorization", cab);
        }
    }
    esp_err_t err = esp_http_client_perform(c);
    int codigo = esp_http_client_get_status_code(c);
    /* El portal de la P4 contesta y cierra: el cliente a veces se queja al
     * final (ESP_ERR_NOT_SUPPORTED) DESPUES de haber leido la respuesta. Si ya
     * tenemos codigo, la peticion ha ido bien y no es un fallo. */
    if (codigo > 0) {
        printf("HTTP %d (%d bytes)\n", codigo,
               (int)esp_http_client_get_content_length(c));
        printf("OK\n");
        esp_http_client_cleanup(c);
        return 0;
    }
    printf("ERR http: %s\n", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return 1;
}

static int cmd_udp(int argc, char **argv)
{
    if (argc < 4) { printf("ERR uso: udp <ip> <puerto> <hex...> [cada_ms] [veces]\n"); return 1; }
    uint8_t datos[512];
    int n = parse_hex(argv[3], datos, (int)sizeof(datos));
    if (n < 0) { printf("ERR hex mal formado\n"); return 1; }
    int cada_ms = (argc > 4) ? atoi(argv[4]) : 0;
    int veces   = (argc > 5) ? atoi(argv[5]) : 1;
    if (n == 0) { printf("ERR no hay datos que mandar\n"); return 1; }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { printf("ERR no puedo abrir el socket\n"); return 1; }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)atoi(argv[2]));
    dst.sin_addr.s_addr = inet_addr(argv[1]);

    for (int i = 0; i < (veces > 0 ? veces : 1); i++) {
        int enviado = sendto(s, datos, n, 0, (struct sockaddr *)&dst, sizeof(dst));
        printf("UDP %d/%d bytes=%d\n", i + 1, veces, enviado);
        if (cada_ms > 0) vTaskDelay(pdMS_TO_TICKS(cada_ms));
    }
    close(s);
    printf("OK\n");
    return 0;
}

/* sat <ssid> <clave> <usuario> <claveportal> | sat informe | sat stop
 *   Hace de cabina: se conecta al AP de la P4, escucha su telemetria UDP y le
 *   manda las mismas peticiones HTTP. */
static int cmd_sat(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "informe")) {
        sat_informe();
        printf("OK\n");
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        sat_parar();
        printf("OK satelite parado\n");
        return 0;
    }
    if (argc >= 5) {
        if (!sat_iniciar(argv[1], argv[2], argv[3], argv[4])) {
            printf("ERR no arranco el satelite\n");
            return 1;
        }
        printf("OK satelite conectando a \"%s\"\n", argv[1]);
        return 0;
    }
    printf("ERR uso: sat <ssid> <clave> <usuario> <claveportal> | sat informe | sat stop\n");
    return 1;
}

/* httppost <url> <usuario> <clave> <cuerpo>
 *   Peticion POST con Basic Auth, para probar los endpoints que reciben JSON.
 *   OJO: los cuerpos de verdad pueden cambiar configuracion (o abrir un viaje);
 *   para probar se manda uno invalido y se comprueba que la P4 lo RECHAZA. */
static int cmd_httppost(int argc, char **argv)
{
    if (argc < 5) { printf("ERR uso: httppost <url> <usuario> <clave> <cuerpo>\n"); return 1; }
    esp_http_client_config_t cfg = { .url = argv[1], .timeout_ms = 8000,
                                     .method = HTTP_METHOD_POST };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { printf("ERR no puedo crear el cliente\n"); return 1; }

    char plano[100], cab[200];
    unsigned char b64[128];
    size_t n = 0;
    snprintf(plano, sizeof(plano), "%.32s:%.64s", argv[2], argv[3]);
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &n,
                              (const unsigned char *)plano, strlen(plano)) == 0) {
        b64[n] = 0;
        snprintf(cab, sizeof(cab), "Basic %s", (char *)b64);
        esp_http_client_set_header(c, "Authorization", cab);
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, argv[4], strlen(argv[4]));

    esp_err_t err = esp_http_client_perform(c);
    int codigo = esp_http_client_get_status_code(c);
    if (codigo > 0) {
        printf("HTTP %d\n", codigo);
        printf("OK\n");
        esp_http_client_cleanup(c);
        return 0;
    }
    printf("ERR http: %s\n", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return 1;
}

/* sim [ble <mac> <clave32> [<mac> <clave32> ...] | stop]
 *   sin argumentos: informe de lo que lleva enviado
 *   ble ...       : empieza a emitir con esos aparatos, rotando los 6 tipos
 *   stop          : para */
static int cmd_sim(int argc, char **argv)
{
    if (argc < 2) { sim_ble_informe(); printf("OK\n"); return 0; }
    if (!strcmp(argv[1], "stop")) {
        sim_ble_parar();
        printf("OK simulacion parada\n");
        return 0;
    }
    if (!strcmp(argv[1], "ritmo")) {
        if (argc < 3) { printf("ERR uso: sim ritmo <ms>\n"); return 1; }
        sim_ble_ritmo(atoi(argv[2]));
        printf("OK ritmo %d ms\n", atoi(argv[2]));
        return 0;
    }
    if (!strcmp(argv[1], "caos")) {
        bool on = (argc > 2) ? (strcmp(argv[2], "off") != 0) : true;
        sim_ble_caos(on);
        printf("OK modo caos %s\n", on ? "ACTIVADO (rota solo cada 2 min)" : "quitado");
        return 0;
    }
    if (!strcmp(argv[1], "fijo")) {
        if (argc >= 5) sim_ble_fijo(true, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        else sim_ble_fijo(false, -1, -1, 0);
        printf("OK modo fijo %s\n", (argc >= 5) ? "ACTIVADO" : "quitado");
        return 0;
    }
    if (!strcmp(argv[1], "extremo")) {
        bool on = (argc > 2) ? (strcmp(argv[2], "off") != 0) : true;
        sim_ble_extremo(on);
        printf("OK modo extremo %s\n", on ? "ACTIVADO (centinelas y maximos)" : "quitado");
        return 0;
    }
    if (!strcmp(argv[1], "ble")) {
        int n = (argc - 2) / 2;
        if (n < 1 || n > 4) {
            printf("ERR uso: sim ble <mac> <clave32hex> [<mac> <clave32hex> ...]\n");
            return 1;
        }
        const char *macs[4], *claves[4];
        for (int k = 0; k < n; k++) { macs[k] = argv[2 + 2 * k]; claves[k] = argv[3 + 2 * k]; }
        if (!sim_ble_iniciar(n, macs, claves)) {
            printf("ERR no arranco: mira la MAC (AA:BB:..) o que la clave tenga 32 hex\n");
            return 1;
        }
        printf("OK emitiendo con %d aparato(s), rotando los 6 tipos de registro\n", n);
        return 0;
    }
    printf("ERR uso: sim [ble <mac> <clave32hex> ... | extremo on|off | stop]\n");
    return 1;
}

static void registrar(const char *nombre, const char *ayuda, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = nombre, .help = ayuda, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

/* ── Arranque ───────────────────────────────────────────────────────────── */
static void ble_sync(void)
{
    s_ble_listo = true;
    printf("BLE listo (direccion por defecto)\n");
}

static void ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    /* La consola se llenaba con los avisos de NimBLE: aqui solo interesa lo
     * que se manda y lo que se recibe, no cada anuncio que arranca. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (nimble_port_init() == ESP_OK) {
        ble_hs_cfg.sync_cb = ble_sync;
        nimble_port_freertos_init(ble_host_task);
    } else {
        printf("ERR no arranca el BLE\n");
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "puente>";
    repl_cfg.max_cmdline_length = 256;
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl) != ESP_OK) {
        printf("ERR no arranca la consola\n");
        return;
    }

    esp_console_register_help_command();
    registrar("status",     "estado del puente", cmd_status);
    registrar("blescan",    "blescan on|off", cmd_blescan);
    registrar("blevictron", "emular un monitor de bateria Victron", cmd_blevictron);
    registrar("bleadv",     "bleadv <mac> <hex...>", cmd_bleadv);
    registrar("bleoff",     "parar la emision BLE", cmd_bleoff);
    registrar("wifiscan",   "listar redes WiFi", cmd_wifiscan);
    registrar("wifista",    "wifista <ssid> <clave>", cmd_wifista);
    registrar("wifioff",    "desconectar el WiFi", cmd_wifioff);
    registrar("wifiip",     "IP actual", cmd_wifiip);
    registrar("httpget",    "httpget <url> [usuario] [clave]", cmd_httpget);
    registrar("httppost",   "httppost <url> <usuario> <clave> <cuerpo>", cmd_httppost);
    registrar("udp",        "udp <ip> <puerto> <hex...> [cada_ms] [veces]", cmd_udp);
    registrar("sim",        "sim [ble ... | extremo on|off | stop]", cmd_sim);
    registrar("sat",        "sat <ssid> <clave> <usuario> <claveportal> | informe | stop", cmd_sat);

    printf("\n=== PUENTE DE PRUEBAS listo. Escribe 'help'. ===\n");
    esp_console_start_repl(repl);
}
