/* config_server_ap.c — ciclo de vida del AP Wi-Fi (radio + timers + cola).
 * Extraido de config_server.c (2026-08-14): esta parte nunca toca httpd_start
 * ni registra handlers, solo levanta/para el Soft-AP y decide cuando el
 * portal HTTP debe arrancar o pararse. El servidor HTTP en si (handlers,
 * registro de URIs, DASHBOARD_HTML) se queda en config_server.c, que expone
 * cfg_http_stop()/cfg_dns_stop() via config_server_internal.h para que esta
 * parte pueda pararlo sin tocar sus estaticos directamente.
 */
#include "config_server.h"
#include "config_server_internal.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_types.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <lwip/inet.h>

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
static esp_timer_handle_t s_ap_off_timer = NULL;

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
            if (!config_server_is_running()) break;   /* ya parado */
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
                cfg_dns_stop();
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
    if (!config_server_is_running()) return;   /* ya parado */
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
    esp_err_t err = esp_timer_create(&args, &s_ap_off_timer);
    if (err != ESP_OK) ESP_LOGW(TAG, "timer ap_auto_off no se creo: %s", esp_err_to_name(err));
}

static void ap_off_timer_arm(void)
{
    if (!s_ap_off_timer) return;
    esp_timer_stop(s_ap_off_timer);   /* idempotente */
    esp_err_t err = esp_timer_start_once(s_ap_off_timer, (uint64_t)AP_AUTO_OFF_MS * 1000);
    if (err != ESP_OK) ESP_LOGW(TAG, "AP auto-off no se pudo armar: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "AP auto-off armado: %d min sin clientes", AP_AUTO_OFF_MS / 60000);
}

/* Rearma el auto-off SIN loggear: se llama en cada peticion autenticada para que
 * el server no se apague mientras la app (u otro cliente) esta sondeando. A los
 * 15 min de la ULTIMA peticion valida se apaga solo (se conserva el ahorro). */
void ap_off_timer_kick(void)
{
    if (!s_ap_off_timer) return;
    esp_timer_stop(s_ap_off_timer);
    esp_err_t err = esp_timer_start_once(s_ap_off_timer, (uint64_t)AP_AUTO_OFF_MS * 1000);
    if (err != ESP_OK) ESP_LOGW(TAG, "AP auto-off no se pudo rearmar: %s", esp_err_to_name(err));
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
    if (!config_server_is_running()) cfg_job_post(CFG_JOB_START);
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
        esp_err_t err = nvs_set_str(h, "password", pass);
        if (err != ESP_OK) ESP_LOGW(TAG, "clave del AP regenerada pero NO se pudo fijar en NVS: %s", esp_err_to_name(err));
        err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG, "clave del AP regenerada pero NO persistio: %s", esp_err_to_name(err));
        else ESP_LOGW(TAG, "clave del AP regenerada (aleatoria): verla en Ajustes -> Wi-Fi");
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
            err = nvs_set_str(h, "ssid", ssid);
            if (err != ESP_OK) ESP_LOGW(TAG, "ssid por defecto no se pudo fijar en NVS: %s", esp_err_to_name(err));
        }
        /* La clave la deja lista config_server_ensure_ap_password(), que corre
         * antes de la UI (ver alli el porque). Aqui solo se lee. */
        nvs_get_str(h, "password", pass, &pl);
        if (nvs_get_u8(h, "enabled", &enabled) != ESP_OK) {
            enabled = 1;
            nvs_set_u8(h, "enabled", enabled);
        }
        err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG, "config wifi (ssid/enabled) no persistio: %s", esp_err_to_name(err));
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
    ESP_LOGI(TAG, "AP cfg: ssid='%s' ch=%d auth=WPA2_PSK",
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
    /* REAPLICAR la configuracion despues de arrancar, y comprobar que ha
     * cuajado.
     *
     * En esp_hosted el AP no lo levanta este chip: lo levanta el C6 por RPC. Y
     * se ha visto (21-ago-2026) que el set_config de ANTES del start devuelve
     * ESP_OK, el esclavo dice "softap started"... y en el aire aparece SU AP de
     * fabrica, "ESP_<MAC>" y ABIERTO, en vez del configurado. O sea que el
     * esclavo rehace su configuracion al arrancar y se come la que le mandamos.
     *
     * Se reaplica y se LEE DE VUELTA para no volver a fiarnos de un ESP_OK que
     * no significa nada: si lo que hay puesto no es lo que pedimos, se dice en
     * el log con todas las letras en vez de dejar al satelite dando vueltas con
     * un "no encuentro la red" que despista. */
    esp_err_t re_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (re_err != ESP_OK) {
        ESP_LOGW(TAG, "reaplicar AP cfg fallo: %s", esp_err_to_name(re_err));
    }

    wifi_config_t leida = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &leida) == ESP_OK) {
        ESP_LOGI(TAG, "AP en la radio: ssid='%s' authmode=%d ch=%d",
                 (const char *)leida.ap.ssid, leida.ap.authmode, leida.ap.channel);
        /* Que no coincida es lo ESPERADO hoy, no una sorpresa: esp_hosted le
         * pone al AP el nombre del chip ("ESP_<MAC>") y lo levanta abierto, se
         * configure lo que se configure. Por eso se avisa como W y no como E, y
         * se dice el nombre de verdad -- que es el que hay que poner en los
         * satelites, y el que llevaba toda la noche despistando porque la
         * pantalla de Ajustes muestra otro. */
        /* Con el C6 en condiciones esto no salta nunca. Si salta, es que ese
         * chip lleva el firmware de FABRICA, que ignora la configuracion y
         * levanta su AP por defecto ("ESP_<MAC>", abierto). Paso el 21-ago-2026
         * en una de las dos pantallas y se arreglo grabandole al C6 el firmware
         * de ~/esp_hosted_slave; la otra ya venia bien. El procedimiento esta en
         * el historial (commit del boton "Actualizar radio C6", retirado despues
         * de usarlo). */
        if (strcmp((const char *)leida.ap.ssid, ssid) != 0) {
            ESP_LOGW(TAG, "El AP NO se llama '%s' sino '%s': el C6 esta ignorando "
                          "la configuracion (firmware de fabrica). Es el nombre que "
                          "tendrian que buscar los satelites mientras siga asi.",
                     ssid, (const char *)leida.ap.ssid);
        }
        if (leida.ap.authmode == WIFI_AUTH_OPEN) {
            ESP_LOGW(TAG, "El AP esta ABIERTO (sin cifrar) pese a configurar WPA2: "
                          "esp_hosted no aplica la clave.");
        }

    } else {
        ESP_LOGW(TAG, "no se pudo releer la config del AP");
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
