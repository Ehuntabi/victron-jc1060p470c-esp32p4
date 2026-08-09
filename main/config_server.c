/* config_server.c */
#include "config_server.h"
#include "config_server_internal.h"
#include "charts_svg.h"
#include "data_export_tar.h"
#include "config_storage.h"
#include "victron_ble.h"
#include "dashboard_state.h"
#include "esp_spiffs.h"
#include "ota_update.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_netif_types.h"
#include "dns_server.h" 
#include <lwip/inet.h>
#include "lvgl.h"
#include "rtc_rx8025t.h"
#include "ui.h"
#include "ui/ausente_mode.h"   /* salida de emergencia del modo ausente por HTTP */
#include "ne185/ne185.h"       /* control de luces/bomba (POST /control) */
#include "frigo.h"             /* control del ventilador (POST /control) */
#include "esp_bsp.h"           /* bsp_display_lock/unlock para tocar LVGL desde httpd */
#include "screenshot.h"        /* screenshot_take_bmp para /captura?n=<i> */
#include "esp_heap_caps.h"     /* heap_caps_free del BMP servido */
#include <sys/time.h>
#include <time.h>
#include "datalogger.h"
#include "battery_history.h"
#include <sys/stat.h>


static const char *TAG = "cfg_srv";

// NVS namespace for Wi-Fi AP settings
#define WIFI_NAMESPACE    "wifi"

/* Handlers idempotentes para WIFI_EVENT_AP_START/STOP — workaround del bug
 * de esp_hosted que dispara cada evento dos veces (esp_wifi_start local +
 * repost desde el slave C6 vía rpc_wrap). Adicionalmente esp_wifi_set_config
 * en modo AP reinicia el AP (STOP+START) y eso tambien duplica eventos.
 *
 * El handler default de esp_wifi (wifi_default_action_ap_start) hace netif_add
 * cada vez -> assert "netif already added". Como wifi_default_action_ap_start
 * es estatica en wifi_default.c, no se puede desregistrar. Solucion: hacemos
 * el setup manual sin esp_netif_create_default_wifi_ap (ver wifi_ap_init) y
 * registramos solo estos handlers como state machine simetrica:
 *   - start solo si !started, luego started=true, action_start
 *   - stop  solo si  started, luego started=false, action_stop (remueve netif)
 * Asi el siguiente start vuelve a tener un netif fresco que añadir.
 */
static volatile bool s_ap_started = false;
/* EventGroup para que wifi_ap_init pueda esperar a que el handler async
 * AP_START haya hecho el action_start (con netif añadido + DHCP server
 * activo). Antes dhcp_set_captiveportal_url se ejecutaba justo después de
 * esp_wifi_start y a veces el netif todavía no estaba listo -> dhcps_start
 * fallaba silenciosamente (ESP_ERROR_CHECK_WITHOUT_ABORT) y el portal
 * cautivo quedaba sin activar. */
#define AP_EVT_STARTED  BIT0
static EventGroupHandle_t s_ap_evt = NULL;

static void cfg_srv_ap_start_idempotent(void *arg, esp_event_base_t base,
                                          int32_t id, void *data)
{
    if (s_ap_started) {
        ESP_LOGD(TAG, "WIFI_EVENT_AP_START duplicado, ignorado");
        return;
    }
    s_ap_started = true;
    /* arg = el esp_netif_t* que registramos al hacer event_handler_register */
    esp_netif_t *netif = (esp_netif_t *)arg;

    /* Replicar lo que hace wifi_default_action_ap_start internamente (lo que
     * nos saltamos al no usar esp_netif_create_default_wifi_ap): registrar el
     * rxcb que pasa los paquetes WiFi al stack lwip. Sin esto el AP asocia
     * clientes pero ningun paquete llega al netif -> el DHCP server jamas
     * recibe los DISCOVER y los clientes quedan "conectando..." sin IP. */
    wifi_netif_driver_t driver = esp_netif_get_io_driver(netif);
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        esp_netif_set_mac(netif, mac);
    }
    esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
    esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref,
                                           esp_netif_netstack_buf_free);

    esp_netif_action_start(netif, base, id, data);
    /* Aviso a wifi_ap_init de que el netif ya está montado y el DHCP
     * server lo gestiona — momento seguro para dhcp_set_captiveportal_url. */
    if (s_ap_evt) xEventGroupSetBits(s_ap_evt, AP_EVT_STARTED);
}
static void cfg_srv_ap_stop_idempotent(void *arg, esp_event_base_t base,
                                         int32_t id, void *data)
{
    if (!s_ap_started) {
        ESP_LOGD(TAG, "WIFI_EVENT_AP_STOP duplicado, ignorado");
        return;
    }
    s_ap_started = false;
    /* Sin esto el lwip netif queda colgado y el siguiente AP_START crashea. */
    esp_netif_action_stop(arg, base, id, data);
    /* Limpiar el testigo de "AP ya levantado". Mientras wifi_ap_init corria una
     * sola vez por arranque daba igual, pero ahora el toggle de Ajustes la
     * reinvoca en caliente: si el bit se quedara puesto del ciclo anterior, la
     * espera de wifi_ap_init volveria al instante y dhcp_set_captiveportal_url
     * correria otra vez ANTES de que el netif estuviera listo — justo el fallo
     * silencioso que este EventGroup existe para evitar. */
    if (s_ap_evt) xEventGroupClearBits(s_ap_evt, AP_EVT_STARTED);
}

/* Auto-off del HTTP server tras 15 min sin NUEVAS asociaciones de cliente.
 *
 * Nota: el WiFi AP NO se apaga (el mini está siempre conectado para recibir
 * los frames UDP del publisher). Solo paramos el servidor HTTP de
 * configuración (192.168.4.1). Para reactivarlo: toggle "AP enabled" en
 * Settings re-invoca wifi_ap_init() + config_server_start() (idempotente).
 *
 * La lógica es: en cada WIFI_EVENT_AP_STACONNECTED reseteamos el timer.
 * Si pasan 15 min sin ningún nuevo STA_CONNECTED → HTTP off. El mini se
 * asocia 1 vez al boot y luego no genera más STA_CONNECTED, así que el
 * temporizador caduca como pretendemos. */
#define AP_AUTO_OFF_MS  (15 * 60 * 1000)
static httpd_handle_t s_httpd = NULL;
static dns_server_handle_t s_dns = NULL;
static esp_timer_handle_t s_ap_off_timer = NULL;

static void ap_off_timer_kick(void);   /* fwd */

/* ── Ciclo de vida del portal/AP: UNA cola, UNA tarea ───────────────────────
 *
 * Arrancar y parar el portal se pedia antes desde tres contextos distintos (la
 * tarea de eventos WiFi en STA_CONNECTED, la tarea esp_timer del auto-off y un
 * callback de LVGL en "Reactivar portal web"), y ninguno se coordinaba con los
 * otros. Dos problemas reales:
 *
 *  1) CARRERA sobre s_httpd. El auto-off publica s_httpd = NULL ANTES del
 *     httpd_stop bloqueante; si justo ahi se asociaba un cliente, el handler de
 *     evento veia NULL y arrancaba un httpd NUEVO mientras el viejo aun tenia el
 *     puerto 80. Con CONFIG_LWIP_SO_REUSE=y el segundo bind tiene exito y quedan
 *     dos listeners solapados.
 *  2) I/O DE FLASH fuera de sitio. config_server_start hace mount_spiffs + varias
 *     lecturas NVS; llamarlo desde la tarea de eventos WiFi la bloquea, y desde
 *     un callback de LVGL congela la UI (el watchdog SW da la UI por colgada a
 *     los 3 fallos seguidos y fuerza reset).
 *
 * Solucion: todas las transiciones se PIDEN encolando un trabajo y las EJECUTA
 * esta unica tarea, en serie. Al ser un solo ejecutor no hace falta mutex: dos
 * transiciones no pueden solaparse por construccion. Los que piden no se
 * bloquean nunca (xQueueSend con timeout 0). */
typedef enum {
    CFG_JOB_START,        /* levantar el portal HTTP si no lo esta */
    CFG_JOB_STOP_HTTP,    /* auto-off: parar HTTP, dejar el AP vivo (el mini usa UDP) */
    CFG_JOB_WIFI_APPLY,   /* aplicar el on/off de Ajustes SIN reiniciar la placa */
} cfg_job_t;

static QueueHandle_t s_job_q = NULL;

/* Encola un trabajo. Nunca bloquea: si la cola esta llena es que ya hay una
 * transicion del mismo tipo pendiente, y perderla es inocuo. */
static void cfg_job_post(cfg_job_t job)
{
    if (!s_job_q) return;
    (void)xQueueSend(s_job_q, &job, 0);
}

/* Para el HTTP dejando el AP en pie. Solo lo llama la tarea de ciclo de vida. */
static void cfg_http_stop(void)
{
    if (!s_httpd) return;
    httpd_handle_t h = s_httpd;
    s_httpd = NULL;
    httpd_stop(h);      /* bloqueante: por eso esto vive en su propia tarea */
}

static void cfg_lifecycle_task(void *arg)
{
    (void)arg;
    cfg_job_t job;
    for (;;) {
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE) continue;

        switch (job) {
        case CFG_JOB_START:
            config_server_start();      /* idempotente */
            break;

        case CFG_JOB_STOP_HTTP: {
            if (!s_httpd) break;        /* ya parado */
            /* El mini C6 esta SIEMPRE asociado al AP (recibe la telemetria UDP por
             * broadcast), asi que "hay algun cliente" contaria SIEMPRE al mini y el
             * portal no se apagaria nunca (testigo verde fijo, se pierde el ahorro).
             * Mantenemos el portal vivo solo si hay ALGUN cliente ADEMAS del mini (el
             * movil con la app): es decir, >= 2 STAs asociados. Si lo apagaramos con el
             * movil aun asociado, este no genera un nuevo STA_CONNECTED y el portal no
             * volveria a arrancar solo -> "conectado pero sin datos". La actividad HTTP
             * tambien lo mantiene vivo aparte, via ap_off_timer_kick en cada peticion. */
            wifi_sta_list_t stas = { 0 };
            esp_err_t err = esp_wifi_ap_get_sta_list(&stas);
            if (err != ESP_OK || stas.num >= 2) {
                /* err != ESP_OK: no pudimos consultar (glitch del RPC a la C6) -> por
                 * seguridad asumimos que puede haber alguien y seguimos vivos. */
                ap_off_timer_kick();
                break;
            }
            ESP_LOGI(TAG, "Auto-off: sin clientes (solo el mini), parando HTTP server");
            cfg_http_stop();
            /* El AP WiFi sigue activo: el mini continúa recibiendo UDP. */
            break;
        }

        case CFG_JOB_WIFI_APPLY: {
            /* Toggle de Ajustes en caliente. wifi_ap_init relee "enabled" de NVS y
             * hace stop o start segun toque; aqui solo acompanamos con lo que ella
             * no toca: el portal HTTP y el DNS del captive portal. */
            uint8_t enabled = 1;
            nvs_handle_t h;
            if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
                if (nvs_get_u8(h, "enabled", &enabled) != ESP_OK) enabled = 1;
                nvs_close(h);
            }
            if (!enabled) {
                /* Orden importante: primero se cierran los servicios que hablan por
                 * la red y luego se baja la radio. Al reves, httpd_stop y
                 * stop_dns_server trabajarian sobre sockets de un netif ya caido. */
                ESP_LOGI(TAG, "Wi-Fi OFF en caliente: parando portal y radio");
                if (s_ap_off_timer) esp_timer_stop(s_ap_off_timer);
                cfg_http_stop();
                if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
                wifi_ap_init();         /* con enabled=0 hace esp_wifi_stop() */
            } else {
                ESP_LOGI(TAG, "Wi-Fi ON en caliente: levantando radio y portal");
                if (wifi_ap_init() == ESP_OK) config_server_start();
            }
            break;
        }
        }
    }
}

/* Arranca la tarea de ciclo de vida. Idempotente. */
static void cfg_lifecycle_ensure(void)
{
    if (s_job_q) return;
    s_job_q = xQueueCreate(4, sizeof(cfg_job_t));
    if (!s_job_q) { ESP_LOGE(TAG, "sin memoria para la cola de ciclo de vida"); return; }
    /* Prioridad 3, la misma que el httpd: no debe preemptar a LVGL (prio 4). */
    if (xTaskCreate(cfg_lifecycle_task, "cfg_life", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no pude crear la tarea de ciclo de vida del portal");
        vQueueDelete(s_job_q);
        s_job_q = NULL;
    }
}

void config_server_request_wifi_apply(void)
{
    cfg_job_post(CFG_JOB_WIFI_APPLY);
}

void config_server_request_start(void)
{
    cfg_job_post(CFG_JOB_START);
}

static void ap_auto_off_cb(void *arg)
{
    (void)arg;
    if (!s_httpd) return;   /* ya parado */
    /* Nada bloqueante en la tarea esp_timer: esp_wifi_ap_get_sta_list es un RPC al
     * C6 por SDIO y httpd_stop espera a que salga la tarea httpd (hasta
     * send_wait_timeout = 30 s si hay una descarga en vuelo). Bloquear aqui
     * paralizaria TODOS los demas timers, incluido el feed de 1 s del modo
     * excedente solar: pasados 3 s sin refresco, frigo_solar_tick marca la
     * telemetria como no fresca y ABRE el rele del frigo. */
    cfg_job_post(CFG_JOB_STOP_HTTP);
}

static void ap_off_timer_ensure(void)
{
    if (s_ap_off_timer) return;
    esp_timer_create_args_t args = {
        .callback = ap_auto_off_cb,
        .name     = "ap_auto_off",
    };
    esp_timer_create(&args, &s_ap_off_timer);
}

static void ap_off_timer_arm(void)
{
    if (!s_ap_off_timer) return;
    esp_timer_stop(s_ap_off_timer);   /* idempotente */
    esp_timer_start_once(s_ap_off_timer, (uint64_t)AP_AUTO_OFF_MS * 1000);
    ESP_LOGI(TAG, "AP auto-off armado: %d min sin clientes", AP_AUTO_OFF_MS / 60000);
}

/* Rearma el auto-off SIN loggear: se llama en cada peticion autenticada para que
 * el server no se apague mientras la app (u otro cliente) esta sondeando. A los
 * 15 min de la ULTIMA peticion valida se apaga solo (se conserva el ahorro). */
static void ap_off_timer_kick(void)
{
    if (!s_ap_off_timer) return;
    esp_timer_stop(s_ap_off_timer);
    esp_timer_start_once(s_ap_off_timer, (uint64_t)AP_AUTO_OFF_MS * 1000);
}

bool config_server_is_running(void)
{
    return s_httpd != NULL;
}

/* Logs visibles cuando un cliente intenta asociarse / desconectarse. En
 * esp_hosted rpc_wrap loggea estos eventos solo a nivel VERBOSE, por eso sin
 * estos handlers los intentos del movil son invisibles en monitor.  */
static void cfg_srv_ap_sta_connected(void *arg, esp_event_base_t base,
                                       int32_t id, void *data)
{
    wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
    ESP_LOGI(TAG, "Cliente CONECTADO: MAC=%02x:%02x:%02x:%02x:%02x:%02x aid=%d",
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
             e->aid);
    /* Cada nueva asociación reabre la ventana de auto-off del HTTP. Si el
     * server estaba parado por inactividad, lo reactivamos — ENCOLANDO el
     * arranque, no llamandolo aqui: config_server_start monta SPIFFS y lee NVS,
     * y esto corre en la tarea de eventos del sistema. */
    if (!s_httpd) cfg_job_post(CFG_JOB_START);
    ap_off_timer_arm();
}
static void cfg_srv_ap_sta_disconnected(void *arg, esp_event_base_t base,
                                          int32_t id, void *data)
{
    wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
    ESP_LOGI(TAG, "Cliente DESCONECTADO: MAC=%02x:%02x:%02x:%02x:%02x:%02x aid=%d",
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
             e->aid);
    /* Al irse un cliente reiniciamos la cuenta atras del auto-off: si era el
     * ultimo, el portal se apagara 15 min despues (ahorro cuando no hay nadie).
     * Si aun quedan clientes, ap_auto_off_cb volvera a rearmar al caducar. */
    ap_off_timer_arm();
}
static void cfg_srv_ap_probe_req(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    wifi_event_ap_probe_req_rx_t *e = (wifi_event_ap_probe_req_rx_t *)data;
    ESP_LOGI(TAG, "Probe REQ rssi=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             e->rssi, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
}

static void dhcp_set_captiveportal_url(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "No AP netif handle");
        return;
    }
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    char ip_str[16];
    inet_ntoa_r(ip_info.ip.addr, ip_str, sizeof(ip_str));
    char uri[32];
    snprintf(uri, sizeof(uri), "http://%s", ip_str);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
#ifdef ESP_NETIF_CAPTIVEPORTAL_URI
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_CAPTIVEPORTAL_URI,
                                           uri, strlen(uri)));
#else
    /* ESP_NETIF_CAPTIVEPORTAL_URI not available on this IDF/target — skipped */
    ESP_LOGI(TAG, "Captive portal DHCP option not supported on this platform");
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
    ESP_LOGI(TAG, "DHCP captive portal URL set to %s", uri);
}

/**
 * @brief   Initialize and/or start the Soft-AP.
 *
 *   - On first call: initializes NVS, TCP/IP stack, event loop and Wi-Fi driver.
 *   - On every call: reads SSID/password/enabled flag from NVS.
 *     • If enabled, configures and starts Soft-AP.
 *     • If disabled, stops Soft-AP.
 */


/* Clave aleatoria para el Wi-Fi de la pantalla. Mismo criterio que la de la web
 * (ver http_auth_init): alfabeto sin caracteres que se confundan al leerlos en
 * la pantalla (sin l, o, 1, 0). 10 chars sobre 32 simbolos = 50 bits. */
static void ap_pass_random(char *out, size_t len)
{
    static const char cs[] = "abcdefghijkmnpqrstuvwxyz23456789";
    size_t n = (len > 11) ? 10 : (len - 1);
    for (size_t i = 0; i < n; i++) out[i] = cs[esp_random() % (sizeof(cs) - 1)];
    out[n] = '\0';
}

/* Deja en NVS una clave de AP valida y no adivinable, migrando las de fabrica.
 *
 * Antes salia de fabrica con "12345678" (la sembraba ui.c) o "victron123": las
 * dos publicas, y este repo es abierto. Cualquiera a tiro de Wi-Fi entraba, y
 * desde dentro se ve TODO el trafico en claro (la clave de la web viaja en cada
 * peticion) y se llega hasta la actualizacion de firmware.
 *
 * Va SEPARADA de wifi_ap_init y se llama ANTES de ui_init a proposito: Ajustes
 * lee la clave UNA sola vez al arrancar, asi que si se generara mas tarde (ya
 * dentro de wifi_ap_init) la pantalla enseniaria la vieja mientras el AP usa la
 * nueva -> te quedas fuera sin poder leerla. 2026-07-26. */
void config_server_ensure_ap_password(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS no disponible: no se puede asegurar la clave del AP");
        return;
    }
    static const char *const debiles[] = { "12345678", "victron123" };
    char pass[65] = {0};
    size_t pl = sizeof(pass);
    bool regenerar = (nvs_get_str(h, "password", pass, &pl) != ESP_OK) ||
                     strlen(pass) < 8;
    for (size_t i = 0; !regenerar && i < sizeof(debiles) / sizeof(debiles[0]); i++) {
        if (strcmp(pass, debiles[i]) == 0) regenerar = true;
    }
    if (regenerar) {
        /* No imprimir la clave: log_capture la persistiria en la SD. */
        ap_pass_random(pass, sizeof(pass));
        nvs_set_str(h, "password", pass);
        nvs_commit(h);
        ESP_LOGW(TAG, "clave del AP regenerada (aleatoria): verla en Ajustes -> Wi-Fi");
    }
    nvs_close(h);
}

esp_err_t wifi_ap_init(void)
{
    static bool subsystems_inited = false;
    static bool wifi_drv_inited  = false;   /* esp_wifi_init separado: reintentable si falla el C6 */
    esp_err_t err;

    /* Lo primero, y ANTES de cualquier salida temprana de esta funcion (AP
     * deshabilitado, C6 que no responde): sin la tarea de ciclo de vida el
     * toggle de Ajustes no tendria quien lo ejecutara y el Wi-Fi se quedaria
     * apagado hasta reiniciar, que es justo lo que queremos evitar. */
    cfg_lifecycle_ensure();

    // 1) One-time subsystems init
    if (!subsystems_inited) {
        // NVS
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
            err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "Erasing NVS and retrying");
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);

        // TCP/IP stack + default event loop (one-time, NO dependen del C6)
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        /* Marcar HECHOS los inits one-time AQUI: si esp_wifi_init (abajo) falla por el
         * C6 y se reintenta wifi_ap_init (toggle en Settings), NO se debe re-ejecutar
         * esp_event_loop_create_default (daria INVALID_STATE -> ESP_ERROR_CHECK aborta,
         * rompiendo el objetivo de R1). El esp_wifi_init va aparte con su propio flag. */
        subsystems_inited = true;
    }

    // Wi-Fi driver (depende del C6/esp_hosted; reintentable, NO abortar el boot)
    if (!wifi_drv_inited) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t we = esp_wifi_init(&wcfg);
        if (we != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init fallo: %s -> sigo SIN WiFi", esp_err_to_name(we));
            return we;
        }
        wifi_drv_inited = true;
    }

    // 2) Load SSID/password/enabled from NVS
    char ssid[33] = {0};
    char pass[65] = {0};
    uint8_t enabled = 1;

    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        size_t sl = sizeof(ssid), pl = sizeof(pass);
        if (nvs_get_str(h, "ssid", ssid, &sl) != ESP_OK || sl <= 1) {
            strcpy(ssid, "VictronConfig");
            nvs_set_str(h, "ssid", ssid);
        }
        /* La clave la deja lista config_server_ensure_ap_password(), que corre
         * antes de la UI (ver alli el porque). Aqui solo se lee. */
        nvs_get_str(h, "password", pass, &pl);
        if (nvs_get_u8(h, "enabled", &enabled) != ESP_OK) {
            enabled = 1;
            nvs_set_u8(h, "enabled", enabled);
        }
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS, using defaults");
    }

    // 3) If disabled, stop Soft-AP
    if (!enabled) {
        ESP_LOGI(TAG, "AP disabled → stopping Soft-AP");
        err = esp_wifi_stop();
        if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGI(TAG, "Soft-AP stopped");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "esp_wifi_stop() failed: %s", esp_err_to_name(err));
        return err;
    }

    // 4) Start or restart Soft-AP
    ESP_LOGI(TAG, "Starting Soft-AP, SSID='%s'", ssid);

    /* WORKAROUND bug esp_hosted: cuando se usa Wi-Fi via esp_hosted (C6 como
     * slave SDIO), el evento WIFI_EVENT_AP_START se dispara DOS veces (una
     * por esp_wifi_start local y otra reposted por rpc_wrap al recibirlo del
     * C6). El handler default de esp_wifi (wifi_default_action_ap_start) hace
     * netif_add cada vez -> assert "netif already added" en la segunda.
     *
     * No se puede desregistrar wifi_default_action_ap_start (es static en
     * wifi_default.c, no exportada). Solucion: hacemos el setup MANUALMENTE,
     * sin pasar por esp_netif_create_default_wifi_ap() ni esp_wifi_set_default_
     * wifi_ap_handlers(), y registramos nuestro propio handler idempotente.
     */
    static esp_netif_t *s_ap_netif = NULL;
    if (!s_ap_netif) {
        esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
        esp_netif_config_t cfg = {
            .base   = &base_cfg,
            .driver = NULL,
            .stack  = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP,
        };
        s_ap_netif = esp_netif_new(&cfg);
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "esp_netif_new(WIFI_AP) failed");
            return ESP_FAIL;
        }
        ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(s_ap_netif));
        /* Handler nuestro, idempotente. No registramos los default handlers
         * de esp_wifi para AP_START / AP_STOP. */
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                    cfg_srv_ap_start_idempotent, s_ap_netif);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                    cfg_srv_ap_stop_idempotent, s_ap_netif);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                    cfg_srv_ap_sta_connected, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                    cfg_srv_ap_sta_disconnected, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_PROBEREQRECVED,
                                    cfg_srv_ap_probe_req, NULL);
    }
    esp_netif_t *ap_netif = s_ap_netif;

    esp_err_t wm = esp_wifi_set_mode(WIFI_MODE_AP);
    if (wm != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode fallo: %s -> sigo SIN WiFi", esp_err_to_name(wm));
        return wm;
    }

    /* Red de seguridad: WPA2 exige 8 chars. Solo se llega aqui si NVS no abrio
     * (arriba siempre queda una clave valida guardada). Aleatoria en RAM: el AP
     * sigue siendo seguro, aunque esta vez no se pueda mostrar en Ajustes. */
    if (strlen(pass) < 8) {
        ap_pass_random(pass, sizeof(pass));
        ESP_LOGW(TAG, "NVS no disponible: clave del AP aleatoria y SIN guardar");
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = strlen(ssid),
            .max_connection = 4,
            /* Canal 1, no 6: en casa del usuario habia 7 redes vecinas con
             * senal fuerte en el 6, y el AP tardaba en aceptar clientes, se
             * caia a los 10-20 s o directamente no dejaba asociarse. El 1 y el
             * 11 son los otros dos canales que no se solapan. Si el 1 tambien
             * diera guerra, probar el 11. */
            .channel        = 1,
            /* Solo WPA2 (sin WPA legacy, más resistente a downgrade). */
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = false },
        }
    };
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';
    strncpy((char*)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.password[sizeof(ap_cfg.ap.password) - 1] = '\0';

    /* pass en DEBUG para no persistir la credencial en los logs de SD/serie */
    ESP_LOGI(TAG, "AP cfg: ssid='%s' ch=%d auth=WPA_WPA2_PSK",
             ssid, ap_cfg.ap.channel);
    ESP_LOGD(TAG, "AP pass configurada (%u chars)", (unsigned)strlen(pass));

    /* IMPORTANTE: set_config ANTES de start. Si invertimos el orden, en
     * esp_hosted el slave dispara un ciclo STOP+START al recibir set_config
     * y la propagacion de SSID/pass queda en un estado raro -> el AP
     * transmite pero los clientes no pueden asociarse. */
    esp_err_t wifi_cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "esp_wifi_set_config result: 0x%x (%s)", wifi_cfg_err, esp_err_to_name(wifi_cfg_err));

    /* Crear EventGroup ANTES de esp_wifi_start para no perder el evento
     * AP_START (que llega async). xEventGroupCreate solo la primera vez. */
    if (!s_ap_evt) s_ap_evt = xEventGroupCreate();
    esp_err_t ws = esp_wifi_start();
    if (ws != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start fallo: %s -> sigo SIN WiFi", esp_err_to_name(ws));
        return ws;
    }

    /* Esperar hasta 2 s a que el handler async termine el action_start
     * (netif añadido + DHCP server up). Sin esto, dhcp_set_captiveportal_url
     * puede correr antes de que el netif esté listo y fallar en silencio. */
    if (s_ap_evt) {
        xEventGroupWaitBits(s_ap_evt, AP_EVT_STARTED,
                            pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    }
    dhcp_set_captiveportal_url();

    /* Arrancar contador auto-off del HTTP server: si en 15 min no llega
     * ningún STA_CONNECTED nuevo, el HTTP se para (el WiFi AP sigue para
     * que el mini reciba UDP). El handler STA_CONNECTED rearma el timer
     * y reactiva el HTTP si lo encuentra parado. */
    ap_off_timer_ensure();
    ap_off_timer_arm();

    ESP_LOGI(TAG, "Soft-AP started");
    return ESP_OK;
}

// Mount SPIFFS partition and list contents
static void mount_spiffs(void) {
    ESP_LOGI(TAG, "Mounting SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted. Total: %d, Used: %d", total, used);

    // Debug: list files
    DIR *dir = opendir("/spiffs");
    if (dir) {
        struct dirent *entry;
        ESP_LOGI(TAG, "SPIFFS contents:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "  %s", entry->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGW(TAG, "Failed to open SPIFFS directory");
    }
}

// Determine MIME type based on file extension
static const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";
    return "application/octet-stream";
}

// Serve a file from SPIFFS at the given URI
static esp_err_t serve_from_spiffs(httpd_req_t *req, const char *uri) {
    /* Defensa en profundidad: SPIFFS tiene namespace plano y .. no escala,
     * pero rechazamos URIs sospechosas igualmente (caracteres de control,
     * '..', '//', NULs) por si mañana se cambia a un VFS jerarquico. */
    if (!uri || uri[0] != '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    size_t ulen = strlen(uri);
    if (ulen == 0 || ulen > 200) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    if (strstr(uri, "..") || strstr(uri, "//")) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < ulen; ++i) {
        unsigned char c = (unsigned char)uri[i];
        if (c < 0x20 || c == 0x7F) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
            return ESP_FAIL;
        }
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);
    ESP_LOGI(TAG, "Serving %s", filepath);
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, get_content_type(filepath));
    httpd_resp_set_hdr(req, "Connection", "close");
    char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f))) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            ESP_LOGW(TAG, "Chunk send failed");
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Script común que sincroniza la hora del móvil con el ESP cada vez que se
 * carga cualquier página del portal. Hace GET /settime?timestamp=... vía
 * <img src> para evitar problemas de CORS/captive portal. */
const char SETTIME_SCRIPT[] =
    "<script>(function(){try{var i=new Image();i.src='/settime?timestamp='"
    "+Math.floor(Date.now()/1000)+'&_='+Math.random();}catch(e){}})();</script>";

/* BasicAuth — credenciales NO hardcoded en el repo.
 *
 * Al primer boot generamos un default por dispositivo:
 *   user = "victron"
 *   pass = 8 chars ALEATORIOS (esp_random). NO derivada de la MAC (el esquema
 *   antiguo "v_<MAC>" era adivinable porque la MAC es el BSSID que se emite).
 * Se persiste en NVS y se muestra en Ajustes -> Wi-Fi (solo en la pantalla
 * fisica) via config_server_get_web_credentials().
 *
 * La cadena "Basic <base64>" se calcula en RAM al arrancar el HTTP server
 * y se guarda en s_auth_header. check_basic_auth() solo compara strings. */
static char s_auth_header[96] = "";

static void http_auth_init(void)
{
    char user[33] = {0};
    char pass[33] = {0};
    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        size_t ul = sizeof(user), pl = sizeof(pass);
        bool need_default_user = (nvs_get_str(h, "http_user", user, &ul) != ESP_OK || ul <= 1);
        bool need_default_pass = (nvs_get_str(h, "http_pass", pass, &pl) != ESP_OK || pl <= 1);
        /* Migracion de seguridad: la pass por defecto ANTIGUA se derivaba de la
         * MAC del AP ("v_XXXXXX"), y esa MAC es el BSSID que se emite en cada
         * beacon -> cualquiera en el AP la podia calcular. Si detectamos ese
         * patron, la tratamos como "por defecto" y la regeneramos aleatoria. */
        if (!need_default_pass) {
            uint8_t mac[6] = {0};
            esp_wifi_get_mac(WIFI_IF_AP, mac);
            char legacy[16];
            snprintf(legacy, sizeof(legacy), "v_%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
            if (strcmp(pass, legacy) == 0) need_default_pass = true;
        }
        if (need_default_user) {
            strcpy(user, "victron");
            nvs_set_str(h, "http_user", user);
        }
        if (need_default_pass) {
            /* Pass ALEATORIA (no derivable de la MAC). Se muestra en
             * Ajustes -> Wi-Fi para que el dueno la vea (solo en la pantalla). */
            static const char cs[] = "abcdefghijkmnpqrstuvwxyz23456789";
            for (int i = 0; i < 8; i++) pass[i] = cs[esp_random() % (sizeof(cs) - 1)];
            pass[8] = '\0';
            nvs_set_str(h, "http_pass", pass);
        }
        nvs_commit(h);
        nvs_close(h);
        if (need_default_user || need_default_pass) {
            /* No imprimir la pass en claro: log_capture la persistiria en la SD.
             * Queda en NVS; se ve/cambia desde Settings. */
            ESP_LOGW(TAG, "HTTP auth DEFAULT generado para user='%s' "
                          "(pass en NVS; verla/cambiarla en Settings)", user);
        }
    }
    /* Si NVS no abrio, user y pass siguen VACIOS: antes se construia igual la
     * cabecera y salia "Basic Og==" (usuario y clave en blanco), o sea que
     * entraba cualquiera. Se deja s_auth_header vacio a proposito: check_basic_auth
     * exige que no lo este, asi que el portal responde 401 a todo. Cerrado por
     * defecto: sin credenciales, no se entra. 2026-07-26. */
    if (user[0] == '\0' || pass[0] == '\0') {
        s_auth_header[0] = '\0';
        ESP_LOGE(TAG, "sin credenciales (NVS no disponible): el portal web queda cerrado");
        return;
    }

    /* Construir cabecera "Basic <base64(user:pass)>" una vez. */
    char up[68];
    int n = snprintf(up, sizeof(up), "%s:%s", user, pass);
    unsigned char b64[96];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                               (const unsigned char *)up, n) == 0) {
        snprintf(s_auth_header, sizeof(s_auth_header), "Basic %.*s",
                 (int)b64_len, (const char *)b64);
    }
}

/* Getter de las credenciales del portal web (para mostrarlas en Ajustes ->
 * Wi-Fi). Lee de NVS; si no hay, deja cadenas vacias. */
void config_server_get_web_credentials(char *user, size_t ulen,
                                       char *pass, size_t plen)
{
    if (user && ulen) user[0] = '\0';
    if (pass && plen) pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    if (user && ulen) { size_t l = ulen; nvs_get_str(h, "http_user", user, &l); }
    if (pass && plen) { size_t l = plen; nvs_get_str(h, "http_pass", pass, &l); }
    nvs_close(h);
}

/* Basic auth del portal: DOS NIVELES (2026-08-07).
 *
 * Nivel abierto (la mayoria de endpoints). El P4 solo levanta Soft-AP
 * (WIFI_MODE_AP, no hay modo estacion), asi que TODO cliente HTTP ha tenido que
 * pasar antes por la clave WPA2 del AP, que es aleatoria de 8+ caracteres y se
 * regenera sola si alguna vez es debil (config_server_ensure_ap_password).
 * Exigir ademas usuario y clave HTTP era una segunda puerta con la misma llave
 * efectiva, y en la practica dejaba a la app fuera: pedia /api/state una vez por
 * segundo y se llevaba 401 tras 401. Por eso este nivel esta abierto.
 *
 * Nivel estricto: /ota, /save y /keys. Sin esto, quien tuviera la clave del
 * Wi-Fi podia REESCRIBIR EL FIRMWARE (/ota) o llevarse y cambiar las claves AES
 * de los Victron (/save, /keys) — o sea que prestar el Wi-Fi a un invitado
 * equivalia a darle control total. La app NO usa ninguno de los tres (solo
 * /api/state, /control, /ausente, /snapshot, /vigilancia y las descargas de
 * /data), asi que cerrarlos no le afecta en absoluto; al navegador se le piden
 * las credenciales una vez y las recuerda. Se ven en Ajustes -> Wi-Fi.
 *
 * Poniendo PORTAL_REQUIRE_BASIC_AUTH a 1 se exige tambien en el nivel abierto
 * (y entonces la app necesitaria credenciales otra vez). */
#define PORTAL_REQUIRE_BASIC_AUTH 0

/* Comprobacion REAL de credenciales. La usan siempre los endpoints peligrosos.
 * No-static: ver comentario de check_basic_auth mas abajo. */
esp_err_t check_basic_auth_strict(httpd_req_t *req)
{
    char auth[96] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                  auth, sizeof(auth));
    if (err == ESP_OK && s_auth_header[0] != '\0' &&
        strcmp(auth, s_auth_header) == 0) {
        ap_off_timer_kick();   /* peticion valida -> mantener el HTTP server vivo */
        return ESP_OK;
    }
    /* Dejar rastro del 401: sin esto un cliente que no manda credenciales (la
     * app con usuario/clave sin configurar, p.ej.) es INVISIBLE en el log —
     * pasa lo mismo que si no hubiera pedido nada, y se diagnostica a ciegas.
     * NUNCA imprimir la cabecera recibida: log_capture la persistiria en la SD.
     * Throttle de 1 s porque la app repregunta en bucle y esto llenaria el log. */
    static int64_t last_warn_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_warn_us > 1000000) {
        last_warn_us = now_us;
        ESP_LOGW(TAG, "401 en %s: %s", req->uri,
                 s_auth_header[0] == '\0' ? "portal cerrado (sin credenciales en NVS)"
                 : err != ESP_OK          ? "el cliente no manda cabecera Authorization"
                                          : "usuario o clave incorrectos");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"Victron Display\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Auth required");
    return ESP_FAIL;
}

/* Nivel abierto: basta con estar dentro del Wi-Fi (ver el bloque de arriba).
 * No-static: declarado en config_server_internal.h para charts_svg.c y
 * data_export_tar.c (via las macros REQUIRE_AUTH/REQUIRE_AUTH_STRICT). */
esp_err_t check_basic_auth(httpd_req_t *req)
{
#if !PORTAL_REQUIRE_BASIC_AUTH
    /* El kick es obligatorio tambien por aqui: es el UNICO que hay por
     * peticion, y sin el el AP se auto-apagaria a los 15 min con la app
     * usandolo. */
    ap_off_timer_kick();
    return ESP_OK;
#else
    return check_basic_auth_strict(req);
#endif
}

/* REQUIRE_AUTH / REQUIRE_AUTH_STRICT: ver config_server_internal.h (movidas
 * ahi para que charts_svg.c y data_export_tar.c tambien puedan usarlas). */

/* Envoltorios de la actualizacion por Wi-Fi: exigen contrasena y delegan en
 * ota_update.c. Nivel ESTRICTO: escribe el firmware entero. */
static esp_err_t handle_ota_page(httpd_req_t *req) {
    REQUIRE_AUTH_STRICT(req);
    return ota_update_page(req);
}
static esp_err_t handle_ota_post(httpd_req_t *req) {
    REQUIRE_AUTH_STRICT(req);
    return ota_update_receive(req);
}

// GET /snapshot -> foto JPEG del ultimo frame de la camara. Requiere auth (expone la
// camara). JPEG por HW (~80-150KB) en vez de BMP 1.58MB: ~10-20x menos latencia
// sobre el AP y sin el malloc de 1.58MB por peticion (que rozaba el suelo de PSRAM).
static esp_err_t handle_snapshot(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    uint8_t *jpg = NULL;
    size_t   len = 0;
    if (!camera_snapshot_jpeg(&jpg, &len)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "camara sin frame todavia");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return r;
}

// GET /vigilancia -> lista las capturas de movimiento; /vigilancia/<id|fichero> -> sirve el JPEG.
/* Galeria de vigilancia.
 *
 * OJO al historial de esto: la galeria nacio leyendo SOLO el anillo en RAM
 * (PSRAM) de camera.c, cuando las capturas no llegaban a la tarjeta. Despues se
 * anadio vig_sd_drain_task, que las vuelca a /sdcard/vigilancia Y LIBERA EL SLOT
 * DEL ANILLO al conseguirlo (camera.c). Nadie actualizo la galeria: cada captura
 * desaparecia de la lista ~300 ms despues de hacerse, asi que la pagina salia
 * casi siempre vacia ("Aun no hay capturas") aunque los JPEG estuvieran
 * perfectamente guardados en la tarjeta. Parecia que la vigilancia no grababa.
 *
 * Ahora se listan las DOS: los ficheros de la tarjeta (el historial de verdad) y
 * lo que siga pendiente de volcar en el anillo. */
#define VIG_MAX     16        /* capturas del anillo en RAM (pendientes de volcar) */
#define VIG_SD_MAX  24        /* ficheros de la tarjeta que se muestran (los mas nuevos) */
#define VIG_SD_DIR_PATH "/sdcard/vigilancia"
#define VIG_NAME_LEN 32       /* "AAAAMMDD_HHMMSS_nnn.jpg" = 24 con el NUL */

/* Nombres de las capturas de la tarjeta, ascendente (el mas nuevo al final).
 * El nombre es AAAAMMDD_HHMMSS_nnn.jpg, asi que ordenar por texto ES ordenar por
 * fecha. Devuelve cuantos hay en la lista; *total_out son los que hay en la
 * carpeta, para poder decir cuantos quedan fuera en vez de truncar en silencio. */
static int vig_sd_list(char names[][VIG_NAME_LEN], int max, int *total_out)
{
    if (total_out) *total_out = 0;
    if (!camera_sd_bus_lock(2000)) return 0;
    DIR *d = opendir(VIG_SD_DIR_PATH);
    if (!d) { camera_sd_bus_unlock(); return 0; }
    int n = 0, total = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        const size_t l = strlen(nm);
        if (l < 5 || l >= VIG_NAME_LEN) continue;
        if (strcmp(nm + l - 4, ".jpg") != 0) continue;
        total++;
        if (n == max) {
            if (strcmp(nm, names[0]) <= 0) continue;   /* mas viejo que todos */
            memmove(names[0], names[1], (size_t)(max - 1) * VIG_NAME_LEN);
            n--;
        }
        int pos = n;
        while (pos > 0 && strcmp(names[pos - 1], nm) > 0) {
            memcpy(names[pos], names[pos - 1], VIG_NAME_LEN);
            pos--;
        }
        snprintf(names[pos], VIG_NAME_LEN, "%s", nm);
        n++;
    }
    closedir(d);
    camera_sd_bus_unlock();
    if (total_out) *total_out = total;
    return n;
}

/* Sirve un JPEG de la tarjeta en trozos, SOLTANDO el cerrojo del bus entre cada
 * uno: si el httpd retiene la SD durante todo el fichero, el GDMA de la camara
 * se queda parado y se acaba en INT WDT. Mismo patron que vig_write_jpeg_sd. */
static esp_err_t vig_sd_send(httpd_req_t *req, const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), VIG_SD_DIR_PATH "/%s", name);
    if (!camera_sd_bus_lock(2000)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "tarjeta ocupada, reintenta");
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "rb");
    camera_sd_bus_unlock();
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "image/jpeg");
    static char buf[4096];   /* estatico: la pila del httpd la comparten mas handlers */
    for (;;) {
        if (!camera_sd_bus_lock(2000)) break;
        const size_t r = fread(buf, 1, sizeof(buf), f);
        camera_sd_bus_unlock();
        if (r == 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
            fclose(f);
            camera_sd_bus_unlock();
            return ESP_FAIL;
        }
        vTaskDelay(1);   /* ceder al GDMA de la camara entre trozos */
    }
    while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
    fclose(f);
    camera_sd_bus_unlock();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
static esp_err_t handle_vigilancia(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    const char *uri = req->uri;
    const char *idstr = NULL;
    if (strncmp(uri, "/vigilancia/", 12) == 0 && uri[12] != '\0') idstr = uri + 12;

    if (idstr) {
        /* Un nombre acabado en .jpg es un fichero de la tarjeta; un numero, una
         * captura del anillo en RAM aun sin volcar. */
        const size_t il = strlen(idstr);
        if (il >= 5 && il < VIG_NAME_LEN && strcmp(idstr + il - 4, ".jpg") == 0) {
            if (strchr(idstr, '/') || strstr(idstr, "..")) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
                return ESP_FAIL;
            }
            return vig_sd_send(req, idstr);
        }
        uint32_t id = (uint32_t)strtoul(idstr, NULL, 10);
        uint8_t *jpg = NULL; size_t jlen = 0;
        if (id == 0 || !camera_vig_fetch(id, &jpg, &jlen)) { httpd_resp_send_404(req); return ESP_FAIL; }
        httpd_resp_set_type(req, "image/jpeg");
        esp_err_t r = httpd_resp_send(req, (const char *)jpg, jlen);
        free(jpg);
        return r;
    }

    /* Listado HTML con miniaturas en linea: primero lo pendiente en RAM (lo mas
     * reciente, aun sin volcar) y luego el historial de la tarjeta. */
    uint32_t ids[VIG_MAX]; time_t ts[VIG_MAX]; size_t lens[VIG_MAX];
    int n = camera_vig_list(ids, ts, lens, VIG_MAX);
    static char sd_names[VIG_SD_MAX][VIG_NAME_LEN];
    int sd_total = 0;
    const int sd_n = vig_sd_list(sd_names, VIG_SD_MAX, &sd_total);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Vigilancia</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
        "padding:12px}h2{margin:8px 0}a{color:#4FC3F7;text-decoration:none}"
        "img{max-width:100%;display:block;margin:6px 0;border:1px solid #333}"
        ".cap{padding:8px 0;border-bottom:1px solid #333}.t{color:#9e9e9e;font-size:13px}"
        "</style></head><body><h2>Capturas de vigilancia</h2>");
    if (n == 0 && sd_n == 0) {
        httpd_resp_sendstr_chunk(req, "<p>Aun no hay capturas. Activa el modo ausente y muevete "
                                      "delante de la camara.</p>");
    } else {
        char line[400];
        for (int i = 0; i < n; i++) {
            struct tm tmv; localtime_r(&ts[i], &tmv);
            char when[40];
            /* R6: si no hay hora fiable (sin RTC/NTP, año <2020) la fecha es basura;
             * mostrarlo claro en vez de un "1970-..." enganoso. */
            if (tmv.tm_year < 120)
                snprintf(when, sizeof(when), "captura #%u (hora no fijada)", (unsigned)ids[i]);
            else
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
            snprintf(line, sizeof(line),
                     "<div class=cap><div class=t>%s &middot; %u KB</div>"
                     "<a href='/vigilancia/%u'><img src='/vigilancia/%u' loading=lazy></a></div>",
                     when, (unsigned)(lens[i] / 1024), (unsigned)ids[i], (unsigned)ids[i]);
            httpd_resp_sendstr_chunk(req, line);
        }
        /* Historial de la tarjeta, del mas nuevo al mas viejo. El nombre lleva la
         * fecha (AAAAMMDD_HHMMSS), asi que se muestra tal cual formateada. */
        for (int i = sd_n - 1; i >= 0; i--) {
            const char *nm = sd_names[i];
            snprintf(line, sizeof(line),
                     "<div class=cap><div class=t>%.4s-%.2s-%.2s %.2s:%.2s:%.2s &middot; tarjeta</div>"
                     "<a href='/vigilancia/%s'><img src='/vigilancia/%s' loading=lazy></a></div>",
                     nm, nm + 4, nm + 6, nm + 9, nm + 11, nm + 13, nm, nm);
            httpd_resp_sendstr_chunk(req, line);
        }
        char foot[240];
        snprintf(foot, sizeof(foot),
                 "<p class=t>%d en la tarjeta (se muestran las %d mas recientes)"
                 " &middot; %d pendientes de volcar en RAM.</p>",
                 sd_total, sd_n, n);
        httpd_resp_sendstr_chunk(req, foot);
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Salida de EMERGENCIA del modo ausente por HTTP (GET /ausente?off): por si el
 * tactil no responde y no se puede hacer el gesto de los 4 toques -> evita quedar
 * con la pantalla negra hasta un corte fisico. Toma lvgl_port_lock porque
 * ausente_request toca LVGL y aqui estamos en la tarea httpd, no en la de LVGL. */
static esp_err_t handle_ausente(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char q[24] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (strstr(q, "off") || strstr(q, "on")) {
        bool on = strstr(q, "on") != NULL;   /* "off" contiene "o" pero no "on" */
        bool done = false;
        if (bsp_display_lock(300)) {
            ausente_request(on);   /* on: cuenta atras+vigilancia; off: cancela/sale */
            bsp_display_unlock();
            done = true;
        }
        httpd_resp_sendstr(req, !done ? "No pude tomar el lock de pantalla, reintenta"
                                : on ? "Modo ausente/vigilancia activado"
                                     : "Modo ausente desactivado");
    } else {
        httpd_resp_sendstr(req, "Usa /ausente?on para activar vigilancia, /ausente?off para salir.");
    }
    return ESP_OK;
}

/* POST /control: control de cargas para la app. body urlencoded:
 *   dev=luz_int|luz_ext|bomba   -> toggle via NE185 (ne185_send_cmd 'i'/'o'/'p')
 *   dev=fan&mode=auto|off|50|100 -> modo del ventilador del frigo (frigo_set_mode)
 * Solo red, no toca DSI. */
static esp_err_t handle_control(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char body[80] = {0};
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;

    /* Aceptar dev/mode tanto en el BODY (form-encoded) como en el QUERY string,
     * para que cualquier cliente funcione (la app Android los envia en el body,
     * pero otros clientes/curl pueden usar el query). */
    char query[80] = {0};
    httpd_req_get_url_query_str(req, query, sizeof query);
    char params[164];
    snprintf(params, sizeof params, "%s&%s", body, query);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    if      (strstr(params, "dev=luz_int")) { ne185_send_cmd('i'); httpd_resp_sendstr(req, "ok luz_int"); }
    else if (strstr(params,"dev=luz_ext")) { ne185_send_cmd('o'); httpd_resp_sendstr(req, "ok luz_ext"); }
    else if (strstr(params,"dev=bomba"))   { ne185_send_cmd('p'); httpd_resp_sendstr(req, "ok bomba"); }
    else if (strstr(params,"dev=fan")) {
        frigo_mode_t m = FRIGO_MODE_AUTO;
        if      (strstr(params, "mode=off")) m = FRIGO_MODE_OFF;
        else if (strstr(params,"mode=50"))   m = FRIGO_MODE_50;
        else if (strstr(params,"mode=100"))  m = FRIGO_MODE_100;
        else if (strstr(params,"mode=auto")) m = FRIGO_MODE_AUTO;
        frigo_set_mode(m);
        httpd_resp_sendstr(req, "ok fan");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "dev? (luz_int|luz_ext|bomba|fan)");
    }
    return ESP_OK;
}

// Handler for GET /
static esp_err_t handle_root(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    ESP_LOGI(TAG, "GET / -> portal landing");
    uint8_t portal_page = 2; /* default ahora: Dashboard */
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "portal_page", &portal_page);
        nvs_close(h);
    }
    const char *target = "/dashboard";
    if      (portal_page == 0) target = "/keys";
    else if (portal_page == 1) target = "/data";
    else                       target = "/dashboard";

    /* Mini HTML que sincroniza la hora del cliente y luego redirige al
     * destino segun configuracion. */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>", -1);
    httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
    char meta[120];
    snprintf(meta, sizeof(meta),
             "<meta http-equiv='refresh' content='0;url=%s'>"
             "</head><body></body></html>", target);
    httpd_resp_send_chunk(req, meta, -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* /keys -> sirve el index.html de configuracion Victron desde SPIFFS */
static esp_err_t handle_keys(httpd_req_t *req)
{
    REQUIRE_AUTH_STRICT(req);   /* muestra las claves AES de los Victron */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return serve_from_spiffs(req, "/index.html");
}

// Static files catch-all
static esp_err_t handle_static(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    return serve_from_spiffs(req, req->uri);
}

/* true si los n primeros caracteres de s son hex. strtol da 0 en no-hex y
 * colaria una MAC/clave AES basura como valida -> validar antes de convertir. */
static bool str_is_hex(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// Handler for POST /save (MAC address and AES key)
static esp_err_t post_save(httpd_req_t *req) {
    REQUIRE_AUTH_STRICT(req);   /* escribe las claves AES de los Victron */
    ESP_LOGV(TAG, "HTTP POST /save");
    size_t len = req->content_len;
    if (!len || len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid length");
        return ESP_FAIL;
    }
    
    char *body = malloc(len + 1);
    if (!body) return ESP_FAIL;
    
    /* Bucle recv: httpd_req_recv puede devolver menos de len -> body truncado.
     * Tope de esperas SEGUIDAS, igual que en ota_update.c: antes se reintentaba
     * sin limite, asi que un cliente que anunciara Content-Length y luego se
     * callara (el movil sale de cobertura a mitad del POST) dejaba este bucle
     * girando para siempre. El httpd tiene UNA sola tarea -> no se pierde una
     * peticion, se queda MUDO el portal entero hasta reiniciar. Con
     * recv_wait_timeout=30 s, 4 esperas son ~2 min de silencio antes de rendirse. */
    const int MAX_ESPERAS = 4;
    int esperas = 0;
    int received = 0;
    while (received < (int)len) {
        int ret = httpd_req_recv(req, body + received, len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= MAX_ESPERAS) continue;
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                ESP_LOGE(TAG, "/save: %d esperas seguidas sin datos, se corta", esperas);
            free(body);
            return ESP_FAIL;
        }
        esperas = 0;   /* han llegado datos: la cuenta de esperas SEGUIDAS se reinicia */
        received += ret;
    }
    body[received] = '\0';
    
    /* LOGD (no LOGI): el body lleva key=<AES Victron> y log_capture persiste
     * los INFO a la SD -> no volcar la clave en claro. */
    ESP_LOGD(TAG, "Received form data: %s", body);
    
    // Parse form data: mac=XXXXXXXXXXXX&key=YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY
    char mac_str[13] = {0};
    char key_str[33] = {0};
    uint8_t mac[6] = {0};
    uint8_t key[16] = {0};
    
    // Extract MAC address
    char *mac_param = strstr(body, "mac=");
    if (!mac_param) {
        ESP_LOGE(TAG, "MAC parameter not found");
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MAC address required");
        return ESP_FAIL;
    }
    
    char *mac_value = mac_param + 4; // Skip "mac="
    char *mac_end = strchr(mac_value, '&');
    int mac_len = mac_end ? (mac_end - mac_value) : strlen(mac_value);
    
    if (mac_len != 12) {
        ESP_LOGE(TAG, "Invalid MAC length: %d", mac_len);
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MAC must be 12 hex characters");
        return ESP_FAIL;
    }
    
    strncpy(mac_str, mac_value, 12);
    mac_str[12] = '\0';
    if (!str_is_hex(mac_str, 12)) {
        ESP_LOGE(TAG, "MAC no es hex");
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MAC must be 12 hex characters");
        return ESP_FAIL;
    }

    // Extract AES key
    char *key_param = strstr(body, "key=");
    if (!key_param) {
        ESP_LOGE(TAG, "Key parameter not found");
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AES key required");
        return ESP_FAIL;
    }
    
    char *key_value = key_param + 4; // Skip "key="
    char *key_end = strchr(key_value, '&');
    int key_len = key_end ? (key_end - key_value) : strlen(key_value);
    
    if (key_len != 32) {
        ESP_LOGE(TAG, "Invalid key length: %d", key_len);
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AES key must be 32 hex characters");
        return ESP_FAIL;
    }
    
    strncpy(key_str, key_value, 32);
    key_str[32] = '\0';
    if (!str_is_hex(key_str, 32)) {
        ESP_LOGE(TAG, "Key no es hex");
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AES key must be 32 hex characters");
        return ESP_FAIL;
    }

    // Parse MAC address from hex string
    for (int i = 0; i < 6; i++) {
        char tmp[3] = { mac_str[i*2], mac_str[i*2+1], 0 };
        mac[i] = strtol(tmp, NULL, 16);
    }
    
    // Parse AES key from hex string
    for (int i = 0; i < 16; i++) {
        char tmp[3] = { key_str[i*2], key_str[i*2+1], 0 };
        key[i] = strtol(tmp, NULL, 16);
    }
    
    ESP_LOGI(TAG, "Parsed MAC address: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    /* clave AES solo en DEBUG: no debe quedar en los logs persistidos en SD */
    ESP_LOGD(TAG, "Parsed AES key:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, key, 16, ESP_LOG_DEBUG);
    
    // Save to Victron devices configuration
    esp_err_t err = add_victron_device(mac, key);
    
    free(body);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save device");
        return ESP_FAIL;
    }
    
    // Reload BLE configuration to include the new device
    victron_ble_reload_device_config();
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h3>Device added successfully!</h3><p>You can add more devices or close this page.</p>", HTTPD_RESP_USE_STRLEN);
    
    return ESP_OK;
}

// --- Dashboard handlers ---
static esp_err_t handle_api_state(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[1408];   /* ampliado: ahora /api/state incluye camper + frigo */
    size_t n = dashboard_state_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n == 0) return httpd_resp_sendstr(req, "{}");
    return httpd_resp_send(req, buf, n);
}

static const char DASHBOARD_HTML[] =
    "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Victron Dashboard</title><style>"
    "body{background:#06080C;color:#fff;font-family:system-ui,sans-serif;margin:0;padding:16px}"
    "h1{color:#FF9800;margin:0 0 12px}"
    "nav{margin-bottom:12px;display:flex;gap:10px;flex-wrap:wrap}"
    "nav a{color:#4FC3F7;text-decoration:none;padding:6px 12px;border:1px solid #2D3340;border-radius:8px;font-size:14px}"
    "nav a:hover{background:#141821}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}"
    ".card{background:#141821;border:2px solid #2D3340;border-radius:14px;padding:16px}"
    ".card h2{margin:0 0 8px;font-size:18px}"
    ".v{font-size:34px;font-weight:bold}"
    ".u{font-size:18px;color:#8A93A6;margin-left:4px}"
    ".sub{color:#8A93A6;font-size:14px;margin-top:4px}"
    ".bat{border-color:#FF9800}.solar{border-color:#00C851}.dcdc{border-color:#4FC3F7}.en{border-color:#FFD54F}"
    ".alarm{color:#FF4444}"
    "</style></head><body>"
    "<nav>"
      "<a href='/dashboard'><b>Dashboard</b></a>"
      "<a href='/data'>Logs</a>"
      "<a href='/keys'>Keys</a>"
    "</nav>"
    "<h1>Victron Dashboard</h1>"
    "<div class='grid'>"
      "<div class='card bat'><h2>Bateria</h2>"
        "<div><span id='soc' class='v'>--</span><span class='u'>%</span></div>"
        "<div class='sub'><span id='bv'>--</span> V &nbsp; <span id='bi'>--</span> A &nbsp; <span id='bw'>--</span> W</div>"
        "<div class='sub'>TTG: <span id='ttg'>--</span></div>"
        "<div id='bal' class='alarm'></div>"
      "</div>"
      "<div class='card solar'><h2>Solar</h2>"
        "<div><span id='pv' class='v'>--</span><span class='u'>W</span></div>"
        "<div class='sub'>Hoy: <span id='yld'>--</span> kWh</div>"
        "<div id='sal' class='alarm'></div>"
      "</div>"
      "<div class='card dcdc'><h2>DC/DC</h2>"
        "<div class='sub'>Entrada: <span id='dvin'>--</span> V</div>"
        "<div class='sub'>Salida:  <span id='dvout'>--</span> V</div>"
        "<div id='dal' class='alarm'></div>"
      "</div>"
      "<div class='card en'><h2>Energia hoy</h2>"
        "<div class='sub'>PV: <span id='epv'>--</span> kWh</div>"
        "<div class='sub'>Cargas: <span id='eld'>--</span> kWh</div>"
      "</div>"
      "<div class='card en'><h2>Trip computer</h2>"
        "<div class='sub'>Activo: <span id='th'>--</span>h <span id='tm'>--</span>m</div>"
        "<div class='sub'>Cargado: <span id='tc'>--</span> kWh (<span id='tca'>--</span> Ah)</div>"
        "<div class='sub'>Consumido: <span id='td'>--</span> kWh (<span id='tda'>--</span> Ah)</div>"
      "</div>"
    "</div>"
    "<script>"
    "function fmt(x,d){return (x===undefined||x===null)?'--':Number(x).toFixed(d);}"
    "async function tick(){"
    "  try{const r=await fetch('/api/state'); const j=await r.json();"
    "  if(j.battery&&j.battery.has){"
    "    document.getElementById('soc').textContent=fmt(j.battery.soc_pct,1);"
    "    document.getElementById('bv').textContent=fmt(j.battery.voltage_v,2);"
    "    document.getElementById('bi').textContent=fmt(j.battery.current_a,2);"
    "    document.getElementById('bw').textContent=fmt(j.battery.power_w,0);"
    "    document.getElementById('ttg').textContent=j.battery.ttg_min>0?(j.battery.ttg_min+'m'):'--';"
    "    document.getElementById('bal').textContent=j.battery.alarm?('ALARMA #'+j.battery.alarm):'';"
    "  }"
    "  if(j.solar&&j.solar.has){"
    "    document.getElementById('pv').textContent=j.solar.pv_w;"
    "    document.getElementById('yld').textContent=fmt(j.solar.yield_today_kwh,2);"
    "    document.getElementById('sal').textContent=j.solar.error?('ERROR #'+j.solar.error):'';"
    "  }"
    "  if(j.dcdc&&j.dcdc.has){"
    "    document.getElementById('dvin').textContent=fmt(j.dcdc.in_v,2);"
    "    document.getElementById('dvout').textContent=fmt(j.dcdc.out_v,2);"
    "    document.getElementById('dal').textContent=j.dcdc.error?('ERROR #'+j.dcdc.error):'';"
    "  }"
    "  if(j.energy_today){"
    "    document.getElementById('epv').textContent=fmt(j.energy_today.pv_kwh,2);"
    "    document.getElementById('eld').textContent=fmt(j.energy_today.loads_kwh,2);"
    "  }"
    "  if(j.trip){"
    "    document.getElementById('th').textContent=j.trip.hours;"
    "    document.getElementById('tm').textContent=String(j.trip.minutes).padStart(2,'0');"
    "    document.getElementById('tc').textContent=fmt(j.trip.charged_kwh,2);"
    "    document.getElementById('tca').textContent=fmt(j.trip.charged_ah,1);"
    "    document.getElementById('td').textContent=fmt(j.trip.discharged_kwh,2);"
    "    document.getElementById('tda').textContent=fmt(j.trip.discharged_ah,1);"
    "  }"
    "  }catch(e){}"
    "}"
    "tick();setInterval(tick,2000);"
    "</script></body></html>";

static esp_err_t handle_dashboard(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, DASHBOARD_HTML);
}

// Error handler for 404 - Not Found
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirecting to captive portal", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "Redirecting %s → /", req->uri);
    return ESP_OK;
}

// Handler for captive portal redirection
static esp_err_t handle_captive_redirect(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal redirect for %s", req->uri);
    // Android /generate_204 — responder 302 para forzar el portal
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Screenshot handler

static esp_err_t handle_settime(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    long ts = 0;
    char buf[128];

    if (req->method == HTTP_GET) {
        /* GET /settime?timestamp=... — leer query string */
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < sizeof(buf)) {
            if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
                char val[32];
                if (httpd_query_key_value(buf, "timestamp", val, sizeof(val)) == ESP_OK) {
                    ts = strtol(val, NULL, 10);
                }
            }
        }
    } else {
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        const char *p = strstr(buf, "timestamp=");
        if (p) sscanf(p, "timestamp=%ld", &ts);
    }
    if (ts > 1000000000L) {
        time_t epoch = (time_t)ts;
        /* El epoch que envía el móvil es Unix UTC. Lo convertimos a hora LOCAL
         * (Madrid, ya configurada en main.c) para guardarla en el RTC, de modo
         * que coincida con cómo la leemos en arranque (mktime sobre hora local). */
        struct tm t_local;
        localtime_r(&epoch, &t_local);
        if (rtc_is_ready()) {
            rtc_set_time(&t_local);
            ESP_LOGI("CFG_SRV", "Hora sincronizada desde movil (local): %04d-%02d-%02d %02d:%02d:%02d",
                     t_local.tm_year + 1900, t_local.tm_mon + 1, t_local.tm_mday,
                     t_local.tm_hour, t_local.tm_min, t_local.tm_sec);
        }
        /* Sincronizar el reloj del sistema con el epoch UTC original */
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
       /* Backup de hora en NVS */
        nvs_handle_t nh;
        if (nvs_open("rtc_backup", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_i64(nh, "epoch", (int64_t)epoch);
            nvs_commit(nh);
            nvs_close(nh);
        }
        /* Refrescar el label del reloj inmediatamente */
        ui_refresh_clock();
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}


/* GET /captura?n=<i> -> navega a la pantalla i, la captura con lv_snapshot y
 * devuelve el BMP como descarga. Sustituye al auto-tour de la SD (intermitente
 * por compartir bus con el C6). Sin auth: es el AP local y solo son capturas. */
static esp_err_t handle_captura(httpd_req_t *req) {
    REQUIRE_AUTH(req);   /* navega la pantalla fisica (ui_tour_goto_screen): exige auth */
    int n = -1;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "n", val, sizeof(val)) == ESP_OK) n = atoi(val);
    }
    const char *name = ui_tour_goto_screen(n);
    if (!name) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n fuera de rango (usa /capturas)");
        return ESP_FAIL;
    }
    uint8_t *bmp = NULL;
    size_t len = 0;
    if (screenshot_take_bmp(&bmp, &len) != ESP_OK || !bmp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "captura fallo");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/bmp");
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%02d_%s.bmp\"", n, name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    esp_err_t e = httpd_resp_send(req, (const char *)bmp, len);
    heap_caps_free(bmp);
    return e;
}

/* GET /capturas -> pagina indice con un enlace por pantalla. */
static esp_err_t handle_capturas(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    int total = ui_tour_screen_count();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;margin:1.2em;line-height:1.9}"
        "a{font-size:1.1em}</style>"
        "<h2>Capturas de la P4</h2>"
        "<p>Pulsa cada enlace para descargar el BMP de esa pantalla "
        "(la P4 navega hasta ella y la fotografia, tarda ~2 s):</p><ol>");
    char buf[160];
    for (int i = 0; i < total; ++i) {
        snprintf(buf, sizeof(buf),
                 "<li><a href='/captura?n=%d'>captura %02d</a></li>", i, i);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "</ol>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Start the HTTP configuration server


esp_err_t config_server_start(void) {
    /* Idempotente: si el server ya está arriba (p.ej. tras auto-off + STA
     * nuevo que lo reactiva) no hacemos nada. */
    if (s_httpd) {
        ESP_LOGD(TAG, "config_server ya activo, skip");
        return ESP_OK;
    }
    /* Inicializar BasicAuth: lee user/pass de NVS o genera default por MAC. */
    http_auth_init();
    mount_spiffs();
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* Prioridad por debajo de LVGL (prio 4): el default del httpd es 5 y ahogaba
     * a la UI durante descargas grandes (.tar) -> el watchdog SW daba LVGL por
     * colgada (3/3) y forzaba reset. Con prio 3 LVGL siempre preempta al httpd.
     * Junto con el yield periodico en handle_tar_dir elimina la asfixia. */
    cfg.task_priority = 3;
    cfg.stack_size = 20480;  /* lv_snapshot en /captura renderiza toda la pantalla
                              * en la tarea del httpd; 8192 se desbordaba y colgaba */
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 30;
    cfg.max_open_sockets = 4;
    /* Con solo 4 sockets y recv_wait_timeout=30, los keep-alive del cliente (la
     * app sondea /api/state a 1 Hz, y basta abrir la web en el navegador para
     * sumar mas) se quedan ocupados hasta medio minuto. Sin purga LRU el httpd
     * RECHAZA la 5a conexion en vez de reciclar la mas vieja -> la app da
     * "sin conexion" a rachas aunque el P4 este perfectamente vivo. */
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 40;  /* 34 actuales (32 + solar.tar + viaje.tar) mas
                                 * margen. Estaba a 32: uno de margen. Ojo:
                                 * pasarse del tope hace que el registro falle EN
                                 * SILENCIO (no se comprueba el retorno), y la
                                 * pagina simplemente no responde. 2026-07-26. */
    cfg.max_resp_headers = 16;
    esp_err_t herr = httpd_start(&server, &cfg);
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start fallo: %s (no reboot)", esp_err_to_name(herr));
        return herr;
    }
    s_httpd = server;   /* publicar tras start exitoso */

    httpd_uri_t uri_root = { .uri = "/",    .method = HTTP_GET,  .handler = handle_root };
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_snapshot = { .uri = "/snapshot", .method = HTTP_GET, .handler = handle_snapshot };
    httpd_register_uri_handler(server, &uri_snapshot);

    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    httpd_register_uri_handler(server, &uri_save);

    /* Actualizacion del firmware por Wi-Fi (ota_update.c). Con contrasena: deja
     * escribir el firmware entero, asi que no puede quedar abierta. */
    httpd_uri_t uri_ota_get = { .uri = "/ota", .method = HTTP_GET, .handler = handle_ota_page };
    httpd_register_uri_handler(server, &uri_ota_get);
    httpd_uri_t uri_ota_post = { .uri = "/ota", .method = HTTP_POST, .handler = handle_ota_post };
    httpd_register_uri_handler(server, &uri_ota_post);

    // Register captive portal handlers BEFORE the catch-all!
    httpd_uri_t uri_generate_204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive_redirect };
    httpd_register_uri_handler(server, &uri_generate_204);

    httpd_uri_t uri_hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive_redirect };
    httpd_register_uri_handler(server, &uri_hotspot);

    httpd_uri_t uri_ncsi = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = handle_captive_redirect };
    httpd_register_uri_handler(server, &uri_ncsi);

    // Now register the catch-all static handler LAST
    httpd_uri_t uri_settime = { .uri = "/settime", .method = HTTP_POST, .handler = handle_settime };
    httpd_register_uri_handler(server, &uri_settime);
    httpd_uri_t uri_settime_get = { .uri = "/settime", .method = HTTP_GET, .handler = handle_settime };
    httpd_register_uri_handler(server, &uri_settime_get);
    httpd_uri_t uri_capturas = { .uri = "/capturas", .method = HTTP_GET, .handler = handle_capturas };
    httpd_register_uri_handler(server, &uri_capturas);
    httpd_uri_t uri_captura = { .uri = "/captura", .method = HTTP_GET, .handler = handle_captura };
    httpd_register_uri_handler(server, &uri_captura);
    httpd_uri_t uri_dashboard = { .uri = "/dashboard", .method = HTTP_GET, .handler = handle_dashboard };
    httpd_register_uri_handler(server, &uri_dashboard);
    httpd_uri_t uri_keys = { .uri = "/keys", .method = HTTP_GET, .handler = handle_keys };
    httpd_register_uri_handler(server, &uri_keys);
    httpd_uri_t uri_api_state = { .uri = "/api/state", .method = HTTP_GET, .handler = handle_api_state };
    httpd_register_uri_handler(server, &uri_api_state);
    /* /mirror y /mirror.bmp eliminados — la info ya está en otras vistas
     * (Dashboard, Logs, Keys). */
    httpd_uri_t uri_data = { .uri = "/data", .method = HTTP_GET, .handler = handle_data_index };
    httpd_register_uri_handler(server, &uri_data);
    httpd_uri_t uri_data_frigo = { .uri = "/data/frigo", .method = HTTP_GET, .handler = handle_data_frigo };
    httpd_register_uri_handler(server, &uri_data_frigo);
    httpd_uri_t uri_data_frigo_csv = { .uri = "/data/frigo.csv", .method = HTTP_GET, .handler = handle_data_frigo_csv };
    httpd_register_uri_handler(server, &uri_data_frigo_csv);
    httpd_uri_t uri_data_ne185v_csv = { .uri = "/data/ne185v.csv", .method = HTTP_GET, .handler = handle_data_ne185v_csv };
    httpd_register_uri_handler(server, &uri_data_ne185v_csv);
    httpd_uri_t uri_data_bat = { .uri = "/data/bateria", .method = HTTP_GET, .handler = handle_data_bateria };
    httpd_register_uri_handler(server, &uri_data_bat);
    httpd_uri_t uri_data_bat_csv = { .uri = "/data/bateria.csv", .method = HTTP_GET, .handler = handle_data_bateria_csv };
    httpd_register_uri_handler(server, &uri_data_bat_csv);
    httpd_uri_t uri_data_frigo_tar = { .uri = "/data/frigo.tar", .method = HTTP_GET, .handler = handle_data_frigo_tar };
    httpd_register_uri_handler(server, &uri_data_frigo_tar);
    httpd_uri_t uri_data_bat_tar = { .uri = "/data/bateria.tar", .method = HTTP_GET, .handler = handle_data_bateria_tar };
    httpd_register_uri_handler(server, &uri_data_bat_tar);
    httpd_uri_t uri_data_solar_tar = { .uri = "/data/solar.tar", .method = HTTP_GET, .handler = handle_data_solar_tar };
    httpd_register_uri_handler(server, &uri_data_solar_tar);
    httpd_uri_t uri_data_cap_tar ={ .uri = "/data/capturas.tar", .method = HTTP_GET, .handler = handle_data_capturas_tar };
    httpd_register_uri_handler(server, &uri_data_cap_tar);
    httpd_uri_t uri_data_vig_tar = { .uri = "/data/vigilancia.tar", .method = HTTP_GET, .handler = handle_data_vigilancia_tar };
    httpd_register_uri_handler(server, &uri_data_vig_tar);
    httpd_uri_t uri_data_cfg_tar = { .uri = "/data/config.tar", .method = HTTP_GET, .handler = handle_data_config_tar };
    httpd_register_uri_handler(server, &uri_data_cfg_tar);
    httpd_uri_t uri_data_logs_tar = { .uri = "/data/logs.tar", .method = HTTP_GET, .handler = handle_data_logs_tar };
    httpd_register_uri_handler(server, &uri_data_logs_tar);
    httpd_uri_t uri_data_viaje_tar = { .uri = "/data/viaje.tar", .method = HTTP_GET, .handler = handle_data_viaje_tar };
    httpd_register_uri_handler(server, &uri_data_viaje_tar);
    httpd_uri_t uri_vig = { .uri = "/vigilancia", .method = HTTP_GET, .handler = handle_vigilancia };
    httpd_register_uri_handler(server, &uri_vig);
    httpd_uri_t uri_vigf = { .uri = "/vigilancia/*", .method = HTTP_GET, .handler = handle_vigilancia };
    httpd_register_uri_handler(server, &uri_vigf);
    httpd_uri_t uri_ausente = { .uri = "/ausente", .method = HTTP_GET, .handler = handle_ausente };
    httpd_register_uri_handler(server, &uri_ausente);
    httpd_uri_t uri_control = { .uri = "/control", .method = HTTP_POST, .handler = handle_control };
    httpd_register_uri_handler(server, &uri_control);

    httpd_uri_t uri_static = { .uri = "/*",  .method = HTTP_GET,  .handler = handle_static };
    httpd_register_uri_handler(server, &uri_static);

    // 404 handler for captive portal
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "HTTP config server running (with captive‐portal redirect)");

    // Start DNS server for captive portal (solo una vez: cada reactivacion
    // re-entraba aqui tirando el handle sin stop -> 2a task EADDRINUSE ->
    // zombie en recvfrom -> fuga de sockets)
    if (!s_dns) {
        dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dns_cfg);
    }

    return ESP_OK;
}
