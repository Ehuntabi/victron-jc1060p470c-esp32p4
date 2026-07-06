/* config_server.c */
#include "config_server.h"
#include "config_storage.h"
#include "victron_ble.h"
#include "dashboard_state.h"
#include "esp_spiffs.h"
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
#include "esp_http_server.h"
#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_netif.h"
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
static esp_timer_handle_t s_ap_off_timer = NULL;

static void ap_auto_off_cb(void *arg)
{
    (void)arg;
    if (!s_httpd) return;   /* ya parado */
    ESP_LOGI(TAG, "Auto-off: 15 min sin nuevas conexiones, parando HTTP server");
    httpd_stop(s_httpd);
    s_httpd = NULL;
    /* El AP WiFi sigue activo: el mini continúa recibiendo UDP. */
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
     * server estaba parado por inactividad, lo reactivamos. */
    if (!s_httpd) config_server_start();
    ap_off_timer_arm();
}
static void cfg_srv_ap_sta_disconnected(void *arg, esp_event_base_t base,
                                          int32_t id, void *data)
{
    wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
    ESP_LOGI(TAG, "Cliente DESCONECTADO: MAC=%02x:%02x:%02x:%02x:%02x:%02x aid=%d",
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
             e->aid);
    /* No tocamos el timer aquí — lo gestiona el handler de CONNECTED. */
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


esp_err_t wifi_ap_init(void)
{
    static bool subsystems_inited = false;
    static bool wifi_drv_inited  = false;   /* esp_wifi_init separado: reintentable si falla el C6 */
    esp_err_t err;

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
        if (nvs_get_str(h, "password", pass, &pl) != ESP_OK) {
            /* NVS sin pass → forzamos fallback "victron123" YA (la regla
             * de "< 8 chars => victron123" se aplica más abajo, pero ahí
             * NVS ya estaría escrita con "" y la web mostraría password
             * vacía aunque el AP usase "victron123" → confusión UX).
             * Persistimos el default directamente. */
            strcpy(pass, "victron123");
            nvs_set_str(h, "password", pass);
        }
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

    /* Forzar password minima 8 chars (requisito WPA2) y por defecto. */
    if (strlen(pass) < 8) {
        strcpy(pass, "victron123");
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = strlen(ssid),
            .max_connection = 4,
            .channel        = 6,
            /* Solo WPA2 (sin WPA legacy, más resistente a downgrade). */
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = false },
        }
    };
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char*)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));

    /* pass en DEBUG para no persistir la credencial en los logs de SD/serie */
    ESP_LOGI(TAG, "AP cfg: ssid='%s' ch=%d auth=WPA_WPA2_PSK",
             ssid, ap_cfg.ap.channel);
    ESP_LOGW(TAG, "AP pass REAL='%s' (temporal para capturas)", pass);

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
static const char SETTIME_SCRIPT[] =
    "<script>(function(){try{var i=new Image();i.src='/settime?timestamp='"
    "+Math.floor(Date.now()/1000)+'&_='+Math.random();}catch(e){}})();</script>";

/* BasicAuth — credenciales NO hardcoded en el repo.
 *
 * Al primer boot generamos un default único por dispositivo:
 *   user = "victron"
 *   pass = "v_" + últimos 6 hex MAC del C6 (ej "v_DC078D")
 * Y lo persistimos en NVS. El user puede cambiar ambas desde Settings.
 * En el monitor del primer arranque se imprime el valor para que lo apuntes.
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
        if (need_default_user) {
            strcpy(user, "victron");
            nvs_set_str(h, "http_user", user);
        }
        if (need_default_pass) {
            uint8_t mac[6] = {0};
            esp_wifi_get_mac(WIFI_IF_AP, mac);
            snprintf(pass, sizeof(pass), "v_%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
            nvs_set_str(h, "http_pass", pass);
        }
        nvs_commit(h);
        nvs_close(h);
        if (need_default_user || need_default_pass) {
            ESP_LOGW(TAG, "HTTP auth DEFAULT generado: user='%s' pass='%s'  "
                          "(cambiar desde Settings, queda en NVS)",
                     user, pass);
        }
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

static esp_err_t check_basic_auth(httpd_req_t *req)
{
    char auth[96] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                  auth, sizeof(auth));
    if (err == ESP_OK && s_auth_header[0] != '\0' &&
        strcmp(auth, s_auth_header) == 0) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"Victron Display\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Auth required");
    return ESP_FAIL;
}

/* Helper macro: pone al inicio de los handlers que exigen auth. Si falla
 * la respuesta 401 ya está enviada — devolvemos ESP_OK para que el http
 * server no reintente ni loggee error. NO aplicar al captive-portal
 * redirect (handle_captive_redirect): rompería la detección de portal. */
#define REQUIRE_AUTH(req) do { \
    if (check_basic_auth(req) != ESP_OK) return ESP_OK; \
} while (0)

// GET /snapshot -> foto JPEG del ultimo frame de la camara. Sin auth (endpoint de
// verificacion). JPEG por HW (~80-150KB) en vez de BMP 1.58MB: ~10-20x menos latencia
// sobre el AP y sin el malloc de 1.58MB por peticion (que rozaba el suelo de PSRAM).
static esp_err_t handle_snapshot(httpd_req_t *req) {
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

// GET /vigilancia -> lista las capturas de movimiento; /vigilancia/<archivo> -> sirve el BMP.
#define VIG_DIR "/sdcard/vigilancia"
/* Galeria de vigilancia servida desde el anillo en RAM (PSRAM) de camera.c.
 * No se usa la SD: el bus SDMMC se satura con la camara + el C6 durante la
 * vigilancia. Las capturas se pierden al reiniciar (encaja con "modo ausente"). */
#define VIG_MAX 16
static esp_err_t handle_vigilancia(httpd_req_t *req) {
    const char *uri = req->uri;
    const char *idstr = NULL;
    if (strncmp(uri, "/vigilancia/", 12) == 0 && uri[12] != '\0') idstr = uri + 12;

    if (idstr) {
        /* Servir una captura concreta por id (entero). */
        uint32_t id = (uint32_t)strtoul(idstr, NULL, 10);
        uint8_t *jpg = NULL; size_t jlen = 0;
        if (id == 0 || !camera_vig_fetch(id, &jpg, &jlen)) { httpd_resp_send_404(req); return ESP_FAIL; }
        httpd_resp_set_type(req, "image/jpeg");
        esp_err_t r = httpd_resp_send(req, (const char *)jpg, jlen);
        free(jpg);
        return r;
    }

    /* Listado HTML con miniaturas en linea. */
    uint32_t ids[VIG_MAX]; time_t ts[VIG_MAX]; size_t lens[VIG_MAX];
    int n = camera_vig_list(ids, ts, lens, VIG_MAX);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Vigilancia</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
        "padding:12px}h2{margin:8px 0}a{color:#4FC3F7;text-decoration:none}"
        "img{max-width:100%;display:block;margin:6px 0;border:1px solid #333}"
        ".cap{padding:8px 0;border-bottom:1px solid #333}.t{color:#9e9e9e;font-size:13px}"
        "</style></head><body><h2>Capturas de vigilancia</h2>");
    if (n == 0) {
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
        char foot[160];
        snprintf(foot, sizeof(foot), "<p class=t>%d capturas en RAM (las mas recientes; "
                                     "se pierden al reiniciar).</p>", n);
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
    char body[80] = {0};
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    if      (strstr(body, "dev=luz_int")) { ne185_send_cmd('i'); httpd_resp_sendstr(req, "ok luz_int"); }
    else if (strstr(body, "dev=luz_ext")) { ne185_send_cmd('o'); httpd_resp_sendstr(req, "ok luz_ext"); }
    else if (strstr(body, "dev=bomba"))   { ne185_send_cmd('p'); httpd_resp_sendstr(req, "ok bomba"); }
    else if (strstr(body, "dev=fan")) {
        frigo_mode_t m = FRIGO_MODE_AUTO;
        if      (strstr(body, "mode=off"))  m = FRIGO_MODE_OFF;
        else if (strstr(body, "mode=50"))   m = FRIGO_MODE_50;
        else if (strstr(body, "mode=100"))  m = FRIGO_MODE_100;
        else if (strstr(body, "mode=auto")) m = FRIGO_MODE_AUTO;
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
    REQUIRE_AUTH(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return serve_from_spiffs(req, "/index.html");
}

// Static files catch-all
static esp_err_t handle_static(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    return serve_from_spiffs(req, req->uri);
}

// Handler for POST /save (MAC address and AES key)
static esp_err_t post_save(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    ESP_LOGV(TAG, "HTTP POST /save");
    size_t len = req->content_len;
    if (!len || len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid length");
        return ESP_FAIL;
    }
    
    char *body = malloc(len + 1);
    if (!body) return ESP_FAIL;
    
    int ret = httpd_req_recv(req, body, len);
    if (ret <= 0) { 
        free(body); 
        return ESP_FAIL; 
    }
    body[ret] = '\0';
    
    ESP_LOGI(TAG, "Received form data: %s", body);
    
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
      "<div class='card dcdc'><h2>AC 220 V</h2>"
        "<div><span id='acw' class='v'>--</span><span class='u'>W</span></div>"
        "<div class='sub'><span id='acv'>--</span> V &nbsp; <span id='aca'>--</span> A &nbsp; <span id='acf'>--</span> Hz</div>"
        "<div class='sub'>PF: <span id='acpf'>--</span> &nbsp; Total: <span id='acwh'>--</span> Wh</div>"
        "<div id='acst' class='sub'></div>"
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
    "  if(j.ac){"
    "    if(j.ac.has){"
    "      document.getElementById('acw').textContent=fmt(j.ac.power_w,0);"
    "      document.getElementById('acv').textContent=fmt(j.ac.voltage_v,1);"
    "      document.getElementById('aca').textContent=fmt(j.ac.current_a,2);"
    "      document.getElementById('acf').textContent=fmt(j.ac.freq_hz,1);"
    "      document.getElementById('acpf').textContent=fmt(j.ac.pf,2);"
    "      document.getElementById('acwh').textContent=j.ac.energy_wh;"
    "      document.getElementById('acst').textContent=j.ac.alarm?'ALARMA':'';"
    "    } else {"
    "      document.getElementById('acst').textContent='PZEM-004T no conectado';"
    "    }"
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
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/index.html");
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


/* --- Helpers data pages --- */
static void get_today_csv_path(const char *subdir, char *out, size_t out_len)
{
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    if (tm_local.tm_year > 100) {
        snprintf(out, out_len, "/sdcard/%s/%04d-%02d-%02d.csv",
                 subdir,
                 (int)(tm_local.tm_year + 1900) & 0xFFFF,
                 (int)(tm_local.tm_mon + 1) & 0xFF,
                 (int)tm_local.tm_mday & 0xFF);
    } else {
        snprintf(out, out_len, "/sdcard/%s/boot.csv", subdir);
    }
}

/* Devuelve la ruta del CSV más reciente en /sdcard/{subdir}/ (por nombre,
 * que sigue formato YYYY-MM-DD.csv y ordena cronológicamente). out[0]=0 si
 * el directorio no existe o no contiene CSVs. */
static void get_latest_csv_path(const char *subdir, char *out, size_t out_len)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/sdcard/%s", subdir);
    DIR *d = opendir(dirpath);
    if (!d) { out[0] = 0; return; }
    char latest[24] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t nl = strlen(name);
        if (nl < 5 || strcmp(name + nl - 4, ".csv") != 0) continue;
        if (strcmp(name, latest) > 0 && nl < sizeof(latest)) {
            strncpy(latest, name, sizeof(latest) - 1);
        }
    }
    closedir(d);
    if (latest[0]) snprintf(out, out_len, "%s/%s", dirpath, latest);
    else out[0] = 0;
}

/* Lee fichero entero a buffer malloc'd. Devuelve NULL si falla. */
static char *read_file_to_buf(const char *path, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (st.st_size <= 0 || st.st_size > 512 * 1024) return NULL; /* limite 512KB */
    /* Serializar el I/O de SD con el GDMA de la camara (no-op si no hay camara).
     * Solo unlock si el lock se consiguio: dar un mutex ajeno = assert de FreeRTOS. */
    bool sdl = camera_sd_bus_lock(3000);
    FILE *f = fopen(path, "rb");
    if (!f) { if (sdl) camera_sd_bus_unlock(); return NULL; }
    char *buf = malloc(st.st_size + 1);
    if (!buf) { fclose(f); if (sdl) camera_sd_bus_unlock(); return NULL; }
    size_t n = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (sdl) camera_sd_bus_unlock();
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}



/* Construye filas tabla y polyline SVG a partir del CSV.
   El CSV tiene header en la primera linea.
   Devuelve dos buffers malloc'd: rows_html y svg_inner. */
/* ── Helpers de gráfico SVG con auto-escala ──────────────────────── */
typedef struct {
    float *x;       /* posición horizontal */
    float *y;       /* valor central / avg */
    float *y_max;   /* opcional: máximo del bin (igual a y si no se usa) */
    float *y_min;   /* opcional: mínimo del bin */
    int n;
    int cap;
} ts_series_t;

static void ts_init(ts_series_t *s, int cap)
{
    s->x     = malloc(sizeof(float) * cap);
    s->y     = malloc(sizeof(float) * cap);
    s->y_max = malloc(sizeof(float) * cap);
    s->y_min = malloc(sizeof(float) * cap);
    s->n = 0;
    s->cap = cap;
}
static void ts_free(ts_series_t *s)
{
    free(s->x); free(s->y); free(s->y_max); free(s->y_min);
    s->x = s->y = s->y_max = s->y_min = NULL;
    s->n = 0;
}
static void ts_push(ts_series_t *s, float x, float y)
{
    if (s->n < s->cap) {
        s->x[s->n] = x; s->y[s->n] = y;
        s->y_max[s->n] = y; s->y_min[s->n] = y;
        s->n++;
    }
}
static void ts_push3(ts_series_t *s, float x, float avg, float mx, float mn)
{
    if (s->n < s->cap) {
        s->x[s->n] = x; s->y[s->n] = avg;
        s->y_max[s->n] = mx; s->y_min[s->n] = mn;
        s->n++;
    }
}

/* Calcular un buen "step" para la cuadrícula dado un rango */
static float nice_step(float range)
{
    if (range <= 0) return 1.0f;
    float pow10 = 1.0f;
    while (range / pow10 >= 10.0f) pow10 *= 10.0f;
    while (range / pow10 < 1.0f)   pow10 /= 10.0f;
    float n = range / pow10;
    if      (n < 1.5f) return 0.2f * pow10;
    else if (n < 3.0f) return 0.5f * pow10;
    else if (n < 7.0f) return 1.0f * pow10;
    else               return 2.0f * pow10;
}

/* Formatear número con precisión adaptativa */
static void fmt_axis(char *out, size_t cap, float v)
{
    float a = v < 0 ? -v : v;
    if (a >= 100)        snprintf(out, cap, "%.0f", v);
    else if (a >= 10)    snprintf(out, cap, "%.1f", v);
    else                 snprintf(out, cap, "%.2f", v);
}

/* Append seguro con formato a 'buf' (tamaño 'cap') a partir de 'sp'. Devuelve
 * el nuevo 'sp'. Sustituye al idiom inseguro 'sp += snprintf(buf+sp, cap-sp,..)':
 * snprintf devuelve lo que HABRIA escrito, no lo truncado, asi que al llenarse
 * el buffer 'sp' superaba 'cap' y el siguiente 'cap - sp' (size_t) hacia
 * underflow a ~SIZE_MAX -> escritura fuera del heap. Aqui clampamos: una vez
 * lleno se devuelve 'cap' y los appends siguientes no escriben nada. */
static size_t svg_vappend(char *buf, size_t cap, size_t sp, const char *fmt, ...)
{
    if (sp >= cap) return cap;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + sp, cap - sp, fmt, ap);
    va_end(ap);
    if (w < 0) return sp;
    if ((size_t)w >= cap - sp) return cap;  /* truncado: marcar buffer lleno */
    return sp + w;
}

static void build_frigo_html(const char *csv,
                             char **rows_html, char **svg_inner)
{
    (void)rows_html;
    *rows_html = NULL;

    size_t svg_cap = 32 * 1024;
    char *svg = malloc(svg_cap);
    if (!svg) { *svg_inner = NULL; return; }
    svg[0] = 0;
    size_t sp = 0;

    /* Contar líneas y reservar series */
    const char *p = csv;
    const char *nl = strchr(p, '\n');
    if (nl) p = nl + 1;
    int total = 0;
    for (const char *q = p; *q; q++) if (*q == '\n') total++;
    if (total < 1) total = 1;
    int cap = total + 4;

    ts_series_t s_aletas = {0}, s_cong = {0}, s_ext = {0}, s_fan = {0};
    ts_init(&s_aletas, cap);
    ts_init(&s_cong, cap);
    ts_init(&s_ext, cap);
    ts_init(&s_fan, cap);
    if (!s_aletas.x || !s_cong.x || !s_ext.x || !s_fan.x) {
        ts_free(&s_aletas); ts_free(&s_cong); ts_free(&s_ext); ts_free(&s_fan);
        free(svg); *svg_inner = NULL; return;
    }

    int idx = 0;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len < 5) { if (!line_end) break; p = line_end + 1; continue; }
        char buf[160] = {0};
        size_t cp = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, p, cp); buf[cp] = 0;
        char *fields[5] = {0};
        int fi = 0;
        if (cp > 0) fields[fi++] = buf;
        for (size_t i = 0; i < cp && fi < 5; ++i) {
            if (buf[i] == ',') { buf[i] = 0; if (fi < 5) fields[fi++] = &buf[i + 1]; }
        }
        if (fi >= 5) {
            float ta = (strcmp(fields[1], "---") == 0) ? -200.0f : atof(fields[1]);
            float tc = (strcmp(fields[2], "---") == 0) ? -200.0f : atof(fields[2]);
            float te = (strcmp(fields[3], "---") == 0) ? -200.0f : atof(fields[3]);
            int   fp = atoi(fields[4]);
            float xpos = (float)idx;
            if (ta > -120.0f) ts_push(&s_aletas, xpos, ta);
            if (tc > -120.0f) ts_push(&s_cong, xpos, tc);
            if (te > -120.0f) ts_push(&s_ext, xpos, te);
            ts_push(&s_fan, xpos, (float)fp);
        }
        idx++;
        if (!line_end) break;
        p = line_end + 1;
    }

    /* Calcular min/max de temperatura entre las 3 series */
    float t_min = 1e30f, t_max = -1e30f;
    ts_series_t *ts_temps[3] = { &s_aletas, &s_cong, &s_ext };
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < ts_temps[k]->n; i++) {
            float v = ts_temps[k]->y[i];
            if (v < t_min) t_min = v;
            if (v > t_max) t_max = v;
        }
    }
    if (t_min > t_max) { t_min = -10.0f; t_max = 10.0f; }
    /* Margen 10 % a cada lado, mínimo 2 */
    float margin = (t_max - t_min) * 0.10f;
    if (margin < 2.0f) margin = 2.0f;
    t_min -= margin; t_max += margin;
    float t_range = t_max - t_min;
    if (t_range < 1.0f) { t_max += 0.5f; t_min -= 0.5f; t_range = t_max - t_min; }

    /* Layout SVG */
    const int W = 800, H = 360;
    const int pad_l = 60, pad_r = 60, pad_t = 16, pad_b = 30;
    const int gw = W - pad_l - pad_r;
    const int gh = H - pad_t - pad_b;
    #define X_FRIGO(xv) (pad_l + (int)((float)gw * (xv) / (float)((total > 1) ? (total - 1) : 1)))
    #define Y_TEMP_AS(t) (pad_t + (int)((float)gh * (t_max - (t)) / t_range))
    #define Y_FAN_AS(f)  (pad_t + (int)((float)gh * (100.0f - (f)) / 100.0f))

    /* Cabecera SVG + grid */
    sp = svg_vappend(svg, svg_cap, sp,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' width='100%%' style='max-width:100%%'>"
        "<rect x='0' y='0' width='%d' height='%d' fill='#111' rx='8'/>",
        W, H, W, H);
    /* Gridlines de temperatura con auto-step */
    float step = nice_step(t_range);
    /* Empezar en múltiplo de step <= t_min */
    float start = (float)((int)(t_min / step) - 1) * step;
    char num[16];
    for (float v = start; v <= t_max + step; v += step) {
        if (v < t_min - step * 0.01f) continue;
        int yy = Y_TEMP_AS(v);
        if (yy < pad_t || yy > H - pad_b) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#222'/>",
            pad_l, yy, W - pad_r, yy);
        fmt_axis(num, sizeof(num), v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#888' font-size='12' text-anchor='end'>%s°</text>",
            pad_l - 4, yy + 4, num);
    }
    /* Eje derecho fan 0/50/100 */
    for (int v = 0; v <= 100; v += 25) {
        int yy = Y_FAN_AS(v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#FFAA00' font-size='11'>%d%%</text>",
            W - pad_r + 4, yy + 4, v);
    }
    /* Ejes principales */
    sp = svg_vappend(svg, svg_cap, sp,
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>"
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>",
        pad_l, pad_t, pad_l, H - pad_b,
        pad_l, H - pad_b, W - pad_r, H - pad_b);

    /* Polylines: dibujar cada serie */
    static const char *colors[4]  = { "#00BFFF", "#FF4444", "#44FF44", "#FFAA00" };
    ts_series_t *all[4]            = { &s_aletas, &s_cong, &s_ext, &s_fan };
    for (int k = 0; k < 4; k++) {
        if (all[k]->n == 0) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<polyline fill='none' stroke='%s' stroke-width='2' points='", colors[k]);
        for (int i = 0; i < all[k]->n; i++) {
            int x = X_FRIGO(all[k]->x[i]);
            int y = (k == 3) ? Y_FAN_AS(all[k]->y[i]) : Y_TEMP_AS(all[k]->y[i]);
            if (sp + 24 >= svg_cap) break;
            sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
        }
        sp = svg_vappend(svg, svg_cap, sp, "'/>");
    }
    sp = svg_vappend(svg, svg_cap, sp, "</svg>");

    ts_free(&s_aletas); ts_free(&s_cong); ts_free(&s_ext); ts_free(&s_fan);
    *svg_inner = svg;
}

static esp_err_t handle_data_frigo(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* Intentar SD: primero el CSV de hoy, luego el más reciente */
    char path[96];
    get_today_csv_path("frigo", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) {
        get_latest_csv_path("frigo", path, sizeof path);
        if (path[0]) csv = read_file_to_buf(path, &csv_len);
    }
    bool from_sd = (csv != NULL);

    /* Fallback RAM */
    if (!csv) {
        csv = datalogger_get_csv();
    }
    if (!csv) {
        httpd_resp_send_chunk(req,
            "<!DOCTYPE html><html><head>", -1);
        httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
        httpd_resp_send_chunk(req,
            "</head><body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
            "<h2>Sin datos</h2><p>No hay datos disponibles ni en SD ni en RAM.</p>"
            "<a href='/data' style='color:#00BFFF'>Volver</a></body></html>", -1);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char *rows_html = NULL, *svg_inner = NULL;
    build_frigo_html(csv, &rows_html, &svg_inner);

    /* Construir respuesta por chunks */
    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Frigo</title>", -1);
    httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
    httpd_resp_send_chunk(req,
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:10px}"
        "h2{text-align:center}"
        ".legend{text-align:center;margin:8px 0;font-size:14px}"
        ".legend span{display:inline-block;margin:0 10px}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;vertical-align:middle;margin-right:4px}"
        "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:13px}"
        "th,td{padding:6px;border-bottom:1px solid #333;text-align:center}"
        "th{background:#222}"
        ".bar{text-align:center;margin:10px 0}"
        ".bar a{color:#00BFFF;text-decoration:none;margin:0 8px}"
        "</style></head><body>"
        "<h2>FRIGO</h2>"
        "<div class='bar'><a href='/data'>&larr; Datos</a> <a href='/data/frigo.csv'>Descargar CSV (hoy)</a> <a href='/data/frigo.tar'>Descargar todo (.tar)</a></div>"
        "<div class='legend'>"
        "<span><i class='dot' style='background:#00BFFF'></i>Aletas</span>"
        "<span><i class='dot' style='background:#FF4444'></i>Congelador</span>"
        "<span><i class='dot' style='background:#44FF44'></i>Exterior</span>"
        "<span><i class='dot' style='background:#FFAA00'></i>Fan%</span>"
        "</div>", -1);

    if (svg_inner) httpd_resp_send_chunk(req, svg_inner, -1);

    /* Indicador origen */
    httpd_resp_send_chunk(req,
        from_sd ? "<p style='text-align:center;color:#888;font-size:12px'>Origen: SD</p>"
                : "<p style='text-align:center;color:#888;font-size:12px'>Origen: RAM</p>", -1);

    httpd_resp_send_chunk(req, "</body></html>", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    free(csv);
    free(rows_html);
    free(svg_inner);
    return ESP_OK;
}

static esp_err_t handle_data_frigo_csv(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[64];
    get_today_csv_path("frigo", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) csv = datalogger_get_csv();
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=frigo.csv");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        httpd_resp_sendstr(req, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n");
    }
    return ESP_OK;
}


/* Construir HTML+SVG bateria.
   CSV bateria: timestamp,source,milli_amps */
static void build_bateria_html(const char *csv,
                               char **rows_html, char **svg_inner)
{
    (void)rows_html;
    *rows_html = NULL;

    /* Buffer SVG amplio para soportar muchos puntos (8640/24h × 4 series) */
    size_t svg_cap = 192 * 1024;
    char *svg = malloc(svg_cap);
    if (!svg) { *svg_inner = NULL; return; }
    svg[0] = 0;
    size_t sp = 0;

    /* Saltar header */
    const char *p = csv;
    const char *nl = strchr(p, '\n');
    if (nl) p = nl + 1;

    /* Contar líneas */
    int total = 0;
    for (const char *q = p; *q; q++) if (*q == '\n') total++;
    if (total < 1) total = 1;
    int cap = total + 4;

    /* Una serie por fuente */
    ts_series_t s_src[BH_SRC_COUNT] = {0};
    for (int i = 0; i < BH_SRC_COUNT; i++) ts_init(&s_src[i], cap);
    for (int i = 0; i < BH_SRC_COUNT; i++) if (!s_src[i].x) {
        for (int j = 0; j < BH_SRC_COUNT; j++) ts_free(&s_src[j]);
        free(svg); *svg_inner = NULL; return;
    }

    int idx = 0;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len < 5) { if (!line_end) break; p = line_end + 1; continue; }
        char buf[160] = {0};
        size_t cp = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, p, cp); buf[cp] = 0;
        /* CSV nuevo: ts,src,avg,max,min — CSV antiguo: ts,src,milli */
        char *fields[5] = {0};
        int fi = 0;
        if (cp > 0) fields[fi++] = buf;
        for (size_t i = 0; i < cp && fi < 5; ++i) {
            if (buf[i] == ',') { buf[i] = 0; if (fi < 5) fields[fi++] = &buf[i + 1]; }
        }
        if (fi >= 3) {
            const char *src = fields[1];
            float avg = atoi(fields[2]) / 1000.0f;
            float mx  = (fi >= 5) ? atoi(fields[3]) / 1000.0f : avg;
            float mn  = (fi >= 5) ? atoi(fields[4]) / 1000.0f : avg;
            int si = -1;
            for (int k = 0; k < BH_SRC_COUNT; ++k) {
                if (strcmp(src, battery_history_source_name((bh_source_t)k)) == 0) {
                    si = k; break;
                }
            }
            if (si >= 0) ts_push3(&s_src[si], (float)idx, avg, mx, mn);
        }
        idx++;
        if (!line_end) break;
        p = line_end + 1;
    }

    /* Auto-escala usando max/min reales (no solo avg) */
    float a_min = 1e30f, a_max = -1e30f;
    for (int k = 0; k < BH_SRC_COUNT; k++) {
        for (int i = 0; i < s_src[k].n; i++) {
            if (s_src[k].y_max[i] > a_max) a_max = s_src[k].y_max[i];
            if (s_src[k].y_min[i] < a_min) a_min = s_src[k].y_min[i];
        }
    }
    if (a_min > a_max) { a_min = -1.0f; a_max = 1.0f; }
    if (a_min > 0) a_min = 0;
    if (a_max < 0) a_max = 0;
    float margin = (a_max - a_min) * 0.10f;
    if (margin < 0.5f) margin = 0.5f;
    a_min -= margin; a_max += margin;
    float a_range = a_max - a_min;
    if (a_range < 1.0f) { a_max += 0.5f; a_min -= 0.5f; a_range = a_max - a_min; }

    const int W = 800, H = 360;
    const int pad_l = 60, pad_r = 20, pad_t = 16, pad_b = 30;
    const int gw = W - pad_l - pad_r;
    const int gh = H - pad_t - pad_b;
    #define X_BAT_AS(xv) (pad_l + (int)((float)gw * (xv) / (float)((total > 1) ? (total - 1) : 1)))
    #define Y_BAT_AS(a)  (pad_t + (int)((float)gh * (a_max - (a)) / a_range))

    sp = svg_vappend(svg, svg_cap, sp,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' width='100%%' style='max-width:100%%'>"
        "<rect x='0' y='0' width='%d' height='%d' fill='#111' rx='8'/>",
        W, H, W, H);
    /* Gridlines auto */
    float step = nice_step(a_range);
    float start = (float)((int)(a_min / step) - 1) * step;
    char num[16];
    for (float v = start; v <= a_max + step; v += step) {
        if (v < a_min - step * 0.01f) continue;
        int yy = Y_BAT_AS(v);
        if (yy < pad_t || yy > H - pad_b) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#222'/>",
            pad_l, yy, W - pad_r, yy);
        fmt_axis(num, sizeof(num), v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#888' font-size='12' text-anchor='end'>%s A</text>",
            pad_l - 4, yy + 4, num);
    }
    /* Eje cero destacado */
    if (a_min < 0 && a_max > 0) {
        int y0 = Y_BAT_AS(0);
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#666' stroke-dasharray='4,4'/>",
            pad_l, y0, W - pad_r, y0);
    }
    /* Ejes */
    sp = svg_vappend(svg, svg_cap, sp,
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>"
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>",
        pad_l, pad_t, pad_l, H - pad_b,
        pad_l, H - pad_b, W - pad_r, H - pad_b);

    static const char *colors[BH_SRC_COUNT] = {
        "#4FC3F7", "#FFD54F", "#FF8A65", "#AED581"
    };
    /* Si hay >1500 puntos, agrupar en bins manteniendo max-de-max,
     * min-de-min y avg-de-avg para no saturar el SVG ni perder picos. */
    #define BAT_MAX_RENDER_PTS 1500
    for (int k = 0; k < BH_SRC_COUNT; k++) {
        int n = s_src[k].n;
        if (n == 0) continue;
        int step = (n > BAT_MAX_RENDER_PTS) ? ((n + BAT_MAX_RENDER_PTS - 1) / BAT_MAX_RENDER_PTS) : 1;

        /* Polígono del rango max-min */
        bool has_range = false;
        for (int i = 0; i < n; i++) {
            if (s_src[k].y_max[i] != s_src[k].y_min[i]) { has_range = true; break; }
        }
        if (has_range) {
            sp = svg_vappend(svg, svg_cap, sp,
                "<polygon fill='%s' fill-opacity='0.18' stroke='none' points='",
                colors[k]);
            /* Subida por max (bin) */
            for (int i = 0; i < n; i += step) {
                int end = (i + step < n) ? i + step : n;
                float mx = s_src[k].y_max[i];
                for (int j = i + 1; j < end; j++) if (s_src[k].y_max[j] > mx) mx = s_src[k].y_max[j];
                int x = X_BAT_AS(s_src[k].x[i]);
                int y = Y_BAT_AS(mx);
                if (sp + 24 >= svg_cap) break;
                sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
            }
            /* Bajada por min (bin) en orden inverso */
            int last_bin_start = ((n - 1) / step) * step;
            for (int i = last_bin_start; i >= 0; i -= step) {
                int end = (i + step < n) ? i + step : n;
                float mn = s_src[k].y_min[i];
                for (int j = i + 1; j < end; j++) if (s_src[k].y_min[j] < mn) mn = s_src[k].y_min[j];
                int x = X_BAT_AS(s_src[k].x[i]);
                int y = Y_BAT_AS(mn);
                if (sp + 24 >= svg_cap) break;
                sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
            }
            sp = svg_vappend(svg, svg_cap, sp, "'/>");
        }
        /* Línea principal del avg (bin) */
        sp = svg_vappend(svg, svg_cap, sp,
            "<polyline fill='none' stroke='%s' stroke-width='2' points='", colors[k]);
        for (int i = 0; i < n; i += step) {
            int end = (i + step < n) ? i + step : n;
            float sum = 0; int cnt = 0;
            for (int j = i; j < end; j++) { sum += s_src[k].y[j]; cnt++; }
            float avg = (cnt > 0) ? sum / cnt : s_src[k].y[i];
            int x = X_BAT_AS(s_src[k].x[i]);
            int y = Y_BAT_AS(avg);
            if (sp + 24 >= svg_cap) break;
            sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
        }
        sp = svg_vappend(svg, svg_cap, sp, "'/>");
    }
    sp = svg_vappend(svg, svg_cap, sp, "</svg>");

    for (int k = 0; k < BH_SRC_COUNT; k++) ts_free(&s_src[k]);
    *svg_inner = svg;
}

static esp_err_t handle_data_bateria(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char path[96];
    get_today_csv_path("bateria", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) {
        get_latest_csv_path("bateria", path, sizeof path);
        if (path[0]) csv = read_file_to_buf(path, &csv_len);
    }
    bool from_sd = (csv != NULL);

    if (!csv) {
        /* Sin RAM accesible para bateria multi-source: avisamos */
        httpd_resp_send_chunk(req,
            "<!DOCTYPE html><html><head>", -1);
        httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
        httpd_resp_send_chunk(req,
            "</head><body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
            "<h2>Sin datos</h2><p>No hay CSV de bateria en SD todavia.</p>"
            "<a href='/data' style='color:#00BFFF'>Volver</a></body></html>", -1);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char *rows_html = NULL, *svg_inner = NULL;
    build_bateria_html(csv, &rows_html, &svg_inner);

    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Bateria</title>", -1);
    httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
    httpd_resp_send_chunk(req,
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:10px}"
        "h2{text-align:center}"
        ".legend{text-align:center;margin:8px 0;font-size:14px}"
        ".legend span{display:inline-block;margin:0 10px}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;vertical-align:middle;margin-right:4px}"
        "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:13px}"
        "th,td{padding:6px;border-bottom:1px solid #333;text-align:center}"
        "th{background:#222}"
        ".bar{text-align:center;margin:10px 0}"
        ".bar a{color:#00BFFF;text-decoration:none;margin:0 8px}"
        "</style></head><body>"
        "<h2>BATERIA</h2>"
        "<div class='bar'><a href='/data'>&larr; Datos</a> <a href='/data/bateria.csv'>Descargar CSV (hoy)</a> <a href='/data/bateria.tar'>Descargar todo (.tar)</a></div>"
        "<div class='legend'>"
        "<span><i class='dot' style='background:#4FC3F7'></i>BatteryMonitor</span>"
        "<span><i class='dot' style='background:#FFD54F'></i>SolarCharger</span>"
        "<span><i class='dot' style='background:#FF8A65'></i>OrionXS</span>"
        "<span><i class='dot' style='background:#AED581'></i>ACCharger</span>"
        "</div>", -1);

    if (svg_inner) httpd_resp_send_chunk(req, svg_inner, -1);

    httpd_resp_send_chunk(req,
        from_sd ? "<p style='text-align:center;color:#888;font-size:12px'>Origen: SD</p>"
                : "<p style='text-align:center;color:#888;font-size:12px'>Origen: RAM</p>", -1);

    httpd_resp_send_chunk(req, "</body></html>", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    free(csv);
    free(rows_html);
    free(svg_inner);
    return ESP_OK;
}

static esp_err_t handle_data_bateria_csv(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[64];
    get_today_csv_path("bateria", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=bateria.csv");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        httpd_resp_sendstr(req, "timestamp,source,milli_amps\n");
    }
    return ESP_OK;
}

static esp_err_t handle_data_index(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Logs</title>"
        "<script>"
        "(function(){try{var i=new Image();i.src='/settime?timestamp='"
        "+Math.floor(Date.now()/1000)+'&_='+Math.random();}catch(e){}})();"
        "</script>"
        "<style>"
        "body{background:#06080C;color:#fff;font-family:system-ui,sans-serif;margin:0;padding:16px}"
        "h1{color:#FF9800;margin:0 0 12px}"
        "nav{margin-bottom:16px;display:flex;gap:10px;flex-wrap:wrap}"
        "nav a{color:#4FC3F7;text-decoration:none;padding:6px 12px;border:1px solid #2D3340;border-radius:8px;font-size:14px}"
        "nav a:hover{background:#141821}"
        ".btn{display:block;margin:14px auto;padding:24px;font-size:20px;"
        "background:#141821;color:#eee;border:2px solid #2D3340;border-radius:14px;"
        "text-decoration:none;max-width:420px;text-align:center}"
        ".btn:active{background:#2D3340}"
        "</style></head><body>"
        "<nav>"
          "<a href='/dashboard'>Dashboard</a>"
          "<a href='/data'><b>Logs</b></a>"
          "<a href='/keys'>Keys</a>"
        "</nav>"
        "<h1>Logs historicos</h1>"
        "<a class='btn' href='/data/frigo'>FRIGO</a>"
        "<a class='btn' href='/data/bateria'>BATERIA</a>"
        "</body></html>";
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

static esp_err_t handle_logs(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *csv = datalogger_get_csv();
    if (!csv) {
        httpd_resp_set_type(req, "text/csv");
        httpd_resp_sendstr(req, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=frigo_log.csv");
    httpd_resp_sendstr(req, csv);
    free(csv);
    return ESP_OK;
}


/* GET /captura?n=<i> -> navega a la pantalla i, la captura con lv_snapshot y
 * devuelve el BMP como descarga. Sustituye al auto-tour de la SD (intermitente
 * por compartir bus con el C6). Sin auth: es el AP local y solo son capturas. */
static esp_err_t handle_captura(httpd_req_t *req) {
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

static esp_err_t handle_screenshot(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp || !disp->driver || !disp->driver->draw_buf || !disp->driver->draw_buf->buf1) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No framebuffer");
        return ESP_FAIL;
    }
    size_t width = disp->driver->hor_res;
    size_t height = disp->driver->ver_res;
    size_t bpp = sizeof(lv_color_t); // usually 2 (RGB565)
    size_t buf_size = width * height * bpp;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char*)disp->driver->draw_buf->buf1, buf_size);
    return ESP_OK;
}

// Start the HTTP configuration server

/* === TAR streaming === */
/* Header TAR USTAR de 512 bytes */
#define TAR_BLOCK 512

static unsigned tar_checksum(const uint8_t *header)
{
    unsigned sum = 0;
    /* Cuando se calcula el checksum, los 8 bytes del campo checksum cuentan como espacios */
    for (int i = 0; i < TAR_BLOCK; ++i) {
        if (i >= 148 && i < 156) sum += ' ';
        else sum += header[i];
    }
    return sum;
}

static void tar_write_octal(char *dst, size_t len, uint64_t val)
{
    /* Octal ASCII alineado a la derecha, terminado en 0 */
    memset(dst, '0', len - 1);
    dst[len - 1] = 0;
    int i = (int)len - 2;
    while (val > 0 && i >= 0) {
        dst[i--] = '0' + (val & 7);
        val >>= 3;
    }
}

/* Construye un header TAR USTAR para un fichero. mtime epoch. */
static void tar_build_header(uint8_t *hdr, const char *name, size_t size, time_t mtime)
{
    memset(hdr, 0, TAR_BLOCK);
    /* name: 100 bytes */
    strncpy((char*)&hdr[0], name, 99);
    /* mode 0644 */
    tar_write_octal((char*)&hdr[100], 8, 0644);
    /* uid/gid 0 */
    tar_write_octal((char*)&hdr[108], 8, 0);
    tar_write_octal((char*)&hdr[116], 8, 0);
    /* size */
    tar_write_octal((char*)&hdr[124], 12, (uint64_t)size);
    /* mtime */
    tar_write_octal((char*)&hdr[136], 12, (uint64_t)mtime);
    /* checksum placeholder spaces */
    memset(&hdr[148], ' ', 8);
    /* typeflag '0' regular file */
    hdr[156] = '0';
    /* magic ustar */
    memcpy(&hdr[257], "ustar", 5);
    memcpy(&hdr[263], "00", 2);
    /* checksum */
    unsigned sum = tar_checksum(hdr);
    char chk[8];
    snprintf(chk, sizeof chk, "%06o", sum);
    memcpy(&hdr[148], chk, 7);
    hdr[155] = ' ';
}

static esp_err_t handle_tar_dir(httpd_req_t *req, const char *src_dir, const char *attach_name)
{
    httpd_resp_set_type(req, "application/x-tar");
    char disp[160];
    /* Construir manualmente para evitar warning de format-truncation */
    strcpy(disp, "attachment; filename=");
    strncat(disp, attach_name, sizeof(disp) - strlen(disp) - 1);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    DIR *dp = opendir(src_dir);
    if (!dp) {
        /* Aun asi devolvemos un TAR vacio (dos bloques cero) */
        uint8_t zeros[TAR_BLOCK * 2] = {0};
        httpd_resp_send_chunk(req, (const char*)zeros, sizeof zeros);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    uint8_t hdr[TAR_BLOCK];
    char *buf = malloc(2048);
    if (!buf) {
        closedir(dp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        char full_path[400];
        snprintf(full_path, sizeof full_path, "%s/%s", src_dir, de->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        if (st.st_size <= 0) continue;

        /* Header */
        tar_build_header(hdr, de->d_name, st.st_size, st.st_mtime);
        if (httpd_resp_send_chunk(req, (const char*)hdr, TAR_BLOCK) != ESP_OK) break;

        /* Contenido en chunks de 2KB */
        /* Cada op de SD bajo el cerrojo; se SUELTA en el envio de red para que la
         * camara interleave. Solo unlock si se consiguio el lock (mutex ajeno = assert). */
        bool sdl = camera_sd_bus_lock(3000);
        FILE *f = fopen(full_path, "rb");
        if (sdl) camera_sd_bus_unlock();
        if (!f) continue;
        size_t remaining = st.st_size;
        while (remaining > 0) {
            size_t to_read = remaining > 2048 ? 2048 : remaining;
            bool sl = camera_sd_bus_lock(3000);
            size_t n = fread(buf, 1, to_read, f);
            if (sl) camera_sd_bus_unlock();
            if (n == 0) break;
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { remaining = 0; break; }
            remaining -= n;
        }
        bool sl2 = camera_sd_bus_lock(3000);
        fclose(f);
        if (sl2) camera_sd_bus_unlock();

        /* Padding hasta multiplo de 512 */
        size_t pad = (TAR_BLOCK - (st.st_size % TAR_BLOCK)) % TAR_BLOCK;
        if (pad > 0) {
            uint8_t zero[TAR_BLOCK] = {0};
            httpd_resp_send_chunk(req, (const char*)zero, pad);
        }
    }
    closedir(dp);
    free(buf);

    /* Final TAR: dos bloques de zeros */
    uint8_t zeros[TAR_BLOCK * 2] = {0};
    httpd_resp_send_chunk(req, (const char*)zeros, sizeof zeros);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_data_frigo_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/frigo", "frigo.tar");
}

static esp_err_t handle_data_bateria_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/bateria", "bateria.tar");
}

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
    cfg.stack_size = 20480;  /* lv_snapshot en /captura renderiza toda la pantalla
                              * en la tarea del httpd; 8192 se desbordaba y colgaba */
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 30;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 32;  /* handlers actuales + /capturas + /captura + margen */
    cfg.max_resp_headers = 16;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    s_httpd = server;   /* publicar tras start exitoso */

    httpd_uri_t uri_root = { .uri = "/",    .method = HTTP_GET,  .handler = handle_root };
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_snapshot = { .uri = "/snapshot", .method = HTTP_GET, .handler = handle_snapshot };
    httpd_register_uri_handler(server, &uri_snapshot);

    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    httpd_register_uri_handler(server, &uri_save);

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
    httpd_uri_t uri_screenshot = { .uri = "/screenshot", .method = HTTP_GET, .handler = handle_screenshot };
    httpd_register_uri_handler(server, &uri_screenshot);
    httpd_uri_t uri_capturas = { .uri = "/capturas", .method = HTTP_GET, .handler = handle_capturas };
    httpd_register_uri_handler(server, &uri_capturas);
    httpd_uri_t uri_captura = { .uri = "/captura", .method = HTTP_GET, .handler = handle_captura };
    httpd_register_uri_handler(server, &uri_captura);
    httpd_uri_t uri_logs = { .uri = "/logs", .method = HTTP_GET, .handler = handle_logs };
    httpd_register_uri_handler(server, &uri_logs);
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
    httpd_uri_t uri_data_bat = { .uri = "/data/bateria", .method = HTTP_GET, .handler = handle_data_bateria };
    httpd_register_uri_handler(server, &uri_data_bat);
    httpd_uri_t uri_data_bat_csv = { .uri = "/data/bateria.csv", .method = HTTP_GET, .handler = handle_data_bateria_csv };
    httpd_register_uri_handler(server, &uri_data_bat_csv);
    httpd_uri_t uri_data_frigo_tar = { .uri = "/data/frigo.tar", .method = HTTP_GET, .handler = handle_data_frigo_tar };
    httpd_register_uri_handler(server, &uri_data_frigo_tar);
    httpd_uri_t uri_data_bat_tar = { .uri = "/data/bateria.tar", .method = HTTP_GET, .handler = handle_data_bateria_tar };
    httpd_register_uri_handler(server, &uri_data_bat_tar);
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

    // Start DNS server for captive portal
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&dns_cfg);

    return ESP_OK;
}
// force wifi debug
// force wifi pass
// force new mac
// force wifi event
// force no static mac
// force wifi cfg log
// force config after start
// force ssid restart
