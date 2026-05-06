/* config_server.c */
#include "config_server.h"
#include "config_storage.h"
#include "victron_ble.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "dns_server.h" 
#include <lwip/inet.h>
#include "lvgl.h"
#include "rtc_rx8025t.h"
#include <sys/time.h>
#include <time.h>


static const char *TAG = "cfg_srv";

// NVS namespace for Wi-Fi AP settings
#define WIFI_NAMESPACE    "wifi"

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
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, ">>> WIFI_EVENT_AP_START <<<");
    } else if (id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, ">>> WIFI_EVENT_AP_STOP <<<");
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, ">>> Station connected to AP <<<");
    } else {
        ESP_LOGI(TAG, ">>> WiFi event: %ld <<<", id);
    }
}


esp_err_t wifi_ap_init(void)
{
    static bool subsystems_inited = false;
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

        // TCP/IP stack + default event loop
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        // Wi-Fi driver
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

        subsystems_inited = true;
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
            pass[0] = '\0';
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

    // This returns a pointer, not an esp_err_t
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap() failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = strlen(ssid),
            .max_connection = 4,
            .channel        = 1,
        }
    };
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    /* Forzar contraseña para que el AP sea visible en todos los dispositivos */
    if (!pass[0]) {
        strcpy(pass, "victron123");
    }
    strncpy((char*)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.channel = 6;

    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t wifi_cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "esp_wifi_set_config result: 0x%x (%s)", wifi_cfg_err, esp_err_to_name(wifi_cfg_err));
    /* Re-arrancar el AP para que el C6 aplique el nuevo SSID */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(esp_wifi_start());

    dhcp_set_captiveportal_url();
    
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

// Handler for GET /
static esp_err_t handle_root(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET / -> serve index.html");
    return serve_from_spiffs(req, "/index.html");
}

// Static files catch-all
static esp_err_t handle_static(httpd_req_t *req) {
    return serve_from_spiffs(req, req->uri);
}

// Handler for POST /save (MAC address and AES key)
static esp_err_t post_save(httpd_req_t *req) {
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
    ESP_LOGI(TAG, "Parsed AES key:"); 
    ESP_LOG_BUFFER_HEX(TAG, key, 16);
    
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
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    /* Esperar formato: "timestamp=1234567890" (Unix timestamp) */
    long ts = 0;
    long offset = 0;
    sscanf(buf, "timestamp=%ld&offset=%ld", &ts, &offset);
    ts += offset;
    if (ts > 1000000000L) {
        struct tm t;
        time_t epoch = (time_t)ts;
        gmtime_r(&epoch, &t);
        if (rtc_is_ready()) {
            rtc_set_time(&t);
            ESP_LOGI("CFG_SRV", "Hora sincronizada desde movil: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        }
        /* Sincronizar también el reloj del sistema */
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_screenshot(httpd_req_t *req) {
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
esp_err_t config_server_start(void) {
    mount_spiffs();
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 30;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 20;
    cfg.max_resp_headers = 16;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t uri_root = { .uri = "/",    .method = HTTP_GET,  .handler = handle_root };
    httpd_register_uri_handler(server, &uri_root);

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
    httpd_uri_t uri_screenshot = { .uri = "/screenshot", .method = HTTP_GET, .handler = handle_screenshot };
    httpd_register_uri_handler(server, &uri_screenshot);

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
