/* config_server.c — portal HTTP: handlers, registro de URIs y ciclo de vida
 * del httpd/DNS. El ciclo de vida del AP Wi-Fi (radio, timers de auto-off,
 * cola de trabajos) vive en config_server_ap.c desde 2026-08-14; este fichero
 * expone cfg_http_stop()/cfg_dns_stop() via config_server_internal.h para que
 * ese ciclo de vida pueda parar el portal sin tocar s_httpd/s_dns
 * directamente. */
#include "config_server.h"
#include "config_server_internal.h"
#include "charts_svg.h"
#include "data_export_tar.h"
#include "config_server_vigilancia.h"
#include "config_server_viaje.h"
#include "config_server_auth.h"
#include "config_storage.h"
#include "victron_ble.h"
#include "data/dashboard_state.h"
#include "esp_spiffs.h"
#include "ota_update.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "camera.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include "esp_netif_ip_addr.h"  /* esp_ip4_addr_t, la necesita dns_server.h */
#include "dns_server.h"
#include "lvgl.h"
#include "rtc_rx8025t.h"
#include "ui.h"
#include "ui/vigilancia/ausente_mode.h"   /* salida de emergencia del modo ausente por HTTP */
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

static httpd_handle_t s_httpd = NULL;
static dns_server_handle_t s_dns = NULL;

bool config_server_is_running(void)
{
    return s_httpd != NULL;
}

/* Para el HTTP dejando el AP en pie. La llama tambien el ciclo de vida del AP
 * (config_server_ap.c) via config_server_internal.h. */
void cfg_http_stop(void)
{
    if (!s_httpd) return;
    httpd_handle_t h = s_httpd;
    s_httpd = NULL;
    httpd_stop(h);      /* bloqueante */
}

/* Para el DNS del captive portal. Mismo motivo que cfg_http_stop: lo pide el
 * ciclo de vida del AP al apagar el Wi-Fi en caliente. */
void cfg_dns_stop(void)
{
    if (!s_dns) return;
    stop_dns_server(s_dns);
    s_dns = NULL;
}

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
            esp_err_t err = nvs_commit(nh);
            if (err != ESP_OK) ESP_LOGW(TAG, "backup de hora (movil) no persistio: %s", esp_err_to_name(err));
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
    /* Apuntes del satelite 3.5" (inicio/fin de viaje y, mas adelante, los
     * registros). Ver main/portal/config_server_viaje.c. */
    httpd_uri_t uri_viaje = { .uri = "/api/viaje", .method = HTTP_POST, .handler = handle_api_viaje };
    httpd_register_uri_handler(server, &uri_viaje);
    /* Lista de viajes con su estado, y el historico con su nombre de verdad. */
    httpd_uri_t uri_viajes = { .uri = "/data/viajes", .method = HTTP_GET, .handler = handle_data_viajes };
    httpd_register_uri_handler(server, &uri_viajes);
    httpd_uri_t uri_hist = { .uri = "/data/historico.tar", .method = HTTP_GET, .handler = handle_data_historico_tar };
    httpd_register_uri_handler(server, &uri_hist);

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
