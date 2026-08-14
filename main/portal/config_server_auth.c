/* config_server_auth.c — Basic Auth del portal + montaje/servido de SPIFFS.
 * Extraido de config_server.c (2026-08-14): el unico enganche con el ciclo de
 * vida del AP es ap_off_timer_kick() (mantener vivo el HTTP server ante
 * peticiones validas), expuesta via config_server_internal.h.
 */
#include "config_server_internal.h"
#include "config_server_auth.h"
#include "config_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "cfg_srv";

#define WIFI_NAMESPACE "wifi"

/* Allow-list en RAM de los ficheros reales de SPIFFS, rellenada una vez en
 * mount_spiffs() desde el mismo listado que ya se hacia para depurar.
 * serve_from_spiffs() la consulta ANTES de tocar flash: SPIFFS es un
 * namespace plano sin indice, y un miss (ruta que no existe, p.ej. las que
 * mandan los probes de captive-portal del movil como /canonical.html) obliga
 * a escanear TODA la particion para poder decir "no esta". Cada operacion de
 * flash congela el otro nucleo (spi_flash_op_block_func); bajo carga
 * concurrente de camara/BLE eso ya disparo el Interrupt WDT una vez
 * (2026-08-13, ver memoria project_ota_reset_a_medias_resuelto, causa #3).
 * d_name de SPIFFS no lleva '/' inicial (raiz), asi que se compara contra
 * uri+1. Tamano de nombre: CONFIG_SPIFFS_OBJ_NAME_LEN (32) en sdkconfig. */
#define SPIFFS_ALLOWLIST_NAME_MAX 32
#define SPIFFS_ALLOWLIST_MAX      48
static char s_spiffs_files[SPIFFS_ALLOWLIST_MAX][SPIFFS_ALLOWLIST_NAME_MAX];
static int  s_spiffs_file_count = 0;

static bool spiffs_allowlist_has(const char *name) {
    for (int i = 0; i < s_spiffs_file_count; i++) {
        if (strcmp(s_spiffs_files[i], name) == 0) return true;
    }
    return false;
}

// Mount SPIFFS partition, list contents and cache the list en RAM
void mount_spiffs(void) {
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

    // Lista los ficheros y los cachea en s_spiffs_files para el allow-list
    DIR *dir = opendir("/spiffs");
    if (dir) {
        struct dirent *entry;
        ESP_LOGI(TAG, "SPIFFS contents:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "  %s", entry->d_name);
            if (s_spiffs_file_count < SPIFFS_ALLOWLIST_MAX) {
                snprintf(s_spiffs_files[s_spiffs_file_count], SPIFFS_ALLOWLIST_NAME_MAX,
                         "%s", entry->d_name);
                s_spiffs_file_count++;
            } else {
                ESP_LOGW(TAG, "allow-list llena (%d), %s ira siempre por flash",
                         SPIFFS_ALLOWLIST_MAX, entry->d_name);
            }
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
esp_err_t serve_from_spiffs(httpd_req_t *req, const char *uri) {
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

    /* Rechazar sin tocar flash lo que no esta en la allow-list de mount_spiffs()
     * (ver comentario ahi). uri+1 porque d_name no lleva '/' inicial. */
    if (!spiffs_allowlist_has(uri + 1)) {
        ESP_LOGW(TAG, "404 sin tocar flash: %s", uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
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

void http_auth_init(void)
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
        esp_err_t err;
        if (need_default_user) {
            strcpy(user, "victron");
            err = nvs_set_str(h, "http_user", user);
            if (err != ESP_OK) ESP_LOGW(TAG, "http_user por defecto no se pudo fijar en NVS: %s", esp_err_to_name(err));
        }
        if (need_default_pass) {
            /* Pass ALEATORIA (no derivable de la MAC). Se muestra en
             * Ajustes -> Wi-Fi para que el dueno la vea (solo en la pantalla). */
            static const char cs[] = "abcdefghijkmnpqrstuvwxyz23456789";
            for (int i = 0; i < 8; i++) pass[i] = cs[esp_random() % (sizeof(cs) - 1)];
            pass[8] = '\0';
            err = nvs_set_str(h, "http_pass", pass);
            if (err != ESP_OK) ESP_LOGW(TAG, "http_pass por defecto no se pudo fijar en NVS: %s", esp_err_to_name(err));
        }
        err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG, "credenciales http_auth no persistieron: %s", esp_err_to_name(err));
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
 * No-static: declarada en config_server_internal.h. */
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
 * No-static: declarada en config_server_internal.h para charts_svg.c y
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
