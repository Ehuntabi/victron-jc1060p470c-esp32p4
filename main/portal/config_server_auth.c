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
#include "lwip/sockets.h"   /* getpeername: freno de auth por IP */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* NIVEL DE ADMINISTRACION: OTRO SECRETO (auditoria de seguridad del
 * 13-oct-2026). /ota (escribir firmware), /keys (las claves AES de los Victron)
 * y /save usaban LA MISMA clave que el resto del portal, la que lleva la app en
 * el movil y el satelite en su NVS: cualquier sitio donde viva esa clave (un
 * movil prestado, una cabina abierta) daba acceso TOTAL, incluido reescribir el
 * firmware. Ahora esos tres endpoints piden un usuario y una clave APARTE.
 *
 * COMPATIBILIDAD: si no hay credenciales de administracion en NVS, se usa la de
 * siempre (s_admin_header copia s_auth_header). Asi una P4 que ya esta en la
 * autocaravana sigue funcionando igual hasta que su dueño le ponga las nuevas
 * desde Ajustes -> Wi-Fi, que es un boton. */
static char s_admin_header[96] = "";

/* Construye "Basic <base64(user:pass)>" en 'dest'. Devuelve si lo ha logrado. */
static bool cabecera_basic(const char *user, const char *pass, char *dest, size_t n)
{
    char up[68];
    int l = snprintf(up, sizeof(up), "%s:%s", user, pass);
    unsigned char b64[96];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                              (const unsigned char *)up, l) != 0) return false;
    snprintf(dest, n, "Basic %.*s", (int)b64_len, (const char *)b64);
    return true;
}

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
            snprintf(user, sizeof(user), "%s", "victron");
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
        /* Y la de administracion TAMBIEN: si no, se quedaba con el valor de la
         * vuelta anterior en RAM y el nivel estricto la seguia aceptando hasta
         * el reinicio (revision del 13-oct-2026). */
        s_admin_header[0] = '\0';
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

    /* ── Credenciales de ADMINISTRACION (ver el comentario de s_admin_header) ──
     * Si no hay, el nivel estricto se queda con las de siempre. */
    char auser[33] = {0}, apass[33] = {0};
    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t ul = sizeof(auser), pl = sizeof(apass);
        if (nvs_get_str(h, "adm_user", auser, &ul) != ESP_OK) auser[0] = 0;
        if (nvs_get_str(h, "adm_pass", apass, &pl) != ESP_OK) apass[0] = 0;
        nvs_close(h);
    }
    if (auser[0] && apass[0]) {
        if (!cabecera_basic(auser, apass, s_admin_header, sizeof(s_admin_header)))
            snprintf(s_admin_header, sizeof(s_admin_header), "%s", s_auth_header);
    } else {
        snprintf(s_admin_header, sizeof(s_admin_header), "%s", s_auth_header);
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

/* Getter de las credenciales de ADMINISTRACION (para verlas en Ajustes). */
void config_server_get_admin_credentials(char *user, size_t ulen,
                                         char *pass, size_t plen)
{
    if (user && ulen) user[0] = '\0';
    if (pass && plen) pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    if (user && ulen) { size_t l = ulen; nvs_get_str(h, "adm_user", user, &l); }
    if (pass && plen) { size_t l = plen; nvs_get_str(h, "adm_pass", pass, &l); }
    nvs_close(h);
}

/* Cambia SOLO las credenciales de administracion (/ota, /keys, /save). */
esp_err_t config_server_set_admin_credentials(const char *user, const char *pass)
{
    if (!user || !pass || !user[0] || !pass[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(user) > 32 || strlen(pass) > 32)  return ESP_ERR_INVALID_ARG;
    if (strlen(pass) < 8) return ESP_ERR_INVALID_ARG;
    if (strchr(user, ':') || strpbrk(user, "\r\n") || strpbrk(pass, "\r\n"))
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_str(h, "adm_user", user);
    if (e == ESP_OK) e = nvs_set_str(h, "adm_pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no he podido guardar las credenciales de administracion: %s",
                 esp_err_to_name(e));
        return e;
    }
    http_auth_init();   /* reconstruye las DOS cabeceras */
    ESP_LOGW(TAG, "credenciales de ADMINISTRACION cambiadas (usuario '%s'): /ota, "
                  "/keys y /save ya solo las aceptan a ellas", user);
    return ESP_OK;
}

/* CAMBIAR las credenciales del portal desde la pantalla (13-oct-2026).
 *
 * POR QUE HACIA FALTA (auditoria de seguridad): las credenciales se crean
 * aleatorias la primera vez que arranca con la NVS vacia, pero NO habia forma
 * de cambiarlas desde ningun sitio. Eso convierte cualquier filtracion -- y ha
 * habido una: la clave de fabrica de las cabinas estuvo publicada en su
 * repositorio -- en permanente: la unica salida era borrar la NVS entera de la
 * P4, que se lleva por delante el Wi-Fi, las claves AES de los Victron y todo lo
 * demas. Con esto, rotar la clave es un boton en Ajustes -> Wi-Fi.
 *
 * Se guarda en NVS y se RECONSTRUYE la cabecera Basic en el acto: s_auth_header
 * se calcula una sola vez en http_auth_init(), asi que sin volver a llamarla el
 * cambio no surtiria efecto hasta el siguiente reinicio (y el usuario se
 * quedaria fuera con la clave nueva en la mano). */
esp_err_t config_server_set_web_credentials(const char *user, const char *pass)
{
    if (!user || !pass || !user[0] || !pass[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(user) > 32 || strlen(pass) > 32)  return ESP_ERR_INVALID_ARG;
    if (strlen(pass) < 8) return ESP_ERR_INVALID_ARG;   /* menos, no es una clave */
    /* Dos puntos en el usuario rompen el esquema Basic ("usuario:clave"), y un
     * salto de linea en cualquiera de los dos romperia la cabecera HTTP. */
    if (strchr(user, ':') || strpbrk(user, "\r\n") || strpbrk(pass, "\r\n"))
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_str(h, "http_user", user);
    if (e == ESP_OK) e = nvs_set_str(h, "http_pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no he podido guardar las credenciales del portal: %s",
                 esp_err_to_name(e));
        return e;
    }
    http_auth_init();
    ESP_LOGW(TAG, "credenciales del portal CAMBIADAS desde la pantalla (usuario '%s')",
             user);
    return ESP_OK;
}

/* Basic auth del portal: DOS NIVELES (2026-08-07).
 *
 * Nivel abierto (la mayoria de endpoints). Estuvo ABIERTO de verdad hasta el
 * 2026-08-21 con este razonamiento: "el P4 solo levanta Soft-AP, asi que TODO
 * cliente HTTP ha tenido que pasar antes por la clave WPA2 del AP; pedir ademas
 * usuario y clave era una segunda puerta con la misma llave, y dejaba a la app
 * fuera (pedia /api/state cada segundo y se comia un 401 detras de otro)".
 *
 * ESA PREMISA ERA FALSA: el AP NO TIENE CLAVE. esp_hosted (el C6 que hace de
 * radio) ignora el SSID y la clave que le manda este chip y levanta la red
 * ABIERTA con su nombre de fabrica. Verificado el 2026-08-21 releyendo la
 * config con esp_wifi_get_config y escaneando desde el satelite; y ya era asi
 * en julio, no es de ahora. Es un problema conocido y sin resolver de
 * esp-hosted (issue #37 de esp-hosted-mcu, abierto desde marzo de 2025;
 * mismo sintoma en arduino-esp32 #9570: "the WiFi AP is always named as
 * ESP_0C... and the wifi auth is open").
 *
 * O sea que la "segunda puerta con la misma llave" era una puerta SIN llave:
 * cualquiera a veinte metros se asociaba sin contrasena y llegaba a la CAMARA
 * DEL INTERIOR (/snapshot, /vigilancia), a /control, a /ausente y a las
 * descargas de /data. Por eso ahora tambien este nivel pide credenciales.
 *
 * Si algun dia el AP va con WPA2 de verdad se puede volver a valorar -- pero
 * entonces con la premisa comprobada, no supuesta.
 *
 * Nivel de ADMINISTRACION: /ota (escribir firmware), /keys y /save (las claves
 * AES de los Victron), y /captura SOLO cuando pide las pantallas sensibles
 * ("wifi" y "victron_keys", que enseñan las claves). Sin esto, quien tuviera la clave del Wi-Fi podia
 * REESCRIBIR EL FIRMWARE o llevarse las claves AES — o sea que prestar el Wi-Fi a
 * un invitado equivalia a darle control total.
 *
 * OJO CON LA APP (corregido el 13-oct-2026, lo cazo la revision adversarial):
 * esto decia que "la app NO usa ninguno de los tres". Es FALSO: la app tiene un
 * boton de actualizar firmware que hace POST /ota. Con la clave de
 * administracion puesta, esa actualizacion desde el movil deja de funcionar (401)
 * y hay que hacerla desde el navegador, que es donde se teclea la clave de
 * administracion (Ajustes -> Wi-Fi de la P4 la enseña). La app lo dice con un
 * mensaje propio en vez de un 401 en crudo (ver victron-app). El resto de la app
 * (estado, control, camara, descargas y los apuntes del satelite) va por el nivel
 * abierto y no le afecta.
 *
 * El endpoint de los apuntes del satelite, POST /api/viaje, es NIVEL ABIERTO a
 * proposito: las cabinas solo tienen las credenciales del portal. Se cambio el
 * 13-oct-2026 despues de que la revision lo cazara (estaba estricto, y generar la
 * clave de administracion dejaba a las cabinas sin poder mandar un apunte).
 *
 * Con PORTAL_REQUIRE_BASIC_AUTH a 1 se exige tambien en el nivel abierto: la
 * app de Android necesita usuario y clave (se ponen una vez en sus ajustes; se
 * leen en Ajustes -> Wi-Fi de la pantalla). A 0 vuelve a quedar abierto. */
#define PORTAL_REQUIRE_BASIC_AUTH 1

/* Compara en tiempo CONSTANTE (no strcmp, que sale en cuanto encuentra la
 * primera diferencia): sobre un AP sin clave de verdad (ver el bloque de
 * arriba, "cualquiera a veinte metros"), un atacante con acceso a la red
 * podria en teoria medir el tiempo de respuesta byte a byte para adivinar la
 * clave sin fuerza bruta a ciegas. Recorre siempre el mismo numero de bytes
 * (el maximo de las dos cadenas) independientemente de donde este la primera
 * diferencia. Detectado auditando el 08-sep-2026. */
static bool auth_equal_ct(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t max = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < max; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(ca ^ cb);
    }
    return diff == 0;
}

/* Freno ante intentos repetidos de credenciales. Sin esto, un atacante en el
 * Wi-Fi probaba usuario/clave tan rapido como el httpd aguantase peticiones:
 * nada lo frenaba salvo el rendimiento del chip.
 *
 * ── REHECHO EL 13-oct-2026 (auditoria de seguridad) ────────────────────────
 *
 * Antes el freno era un vTaskDelay() de hasta 4 s DENTRO del unico worker del
 * httpd (esp_http_server es de UNA sola tarea). Eso convertia el freno en el
 * ataque: cualquiera mandando credenciales malas en bucle dejaba el portal
 * entero mudo -- /api/state para la app del dueño, y los POST de los apuntes,
 * que caducan a los 8 s y hacen fallar el INICIO DE VIAJE sin motivo aparente.
 *
 * Ahora no se duerme NUNCA: se contesta 429 Too Many Requests con Retry-After y
 * se sigue atendiendo a los demas. Y es POR IP (tabla de los ultimos clientes
 * que fallaron), no global: el que se equivoca con su clave no le cierra la
 * puerta a los demas. */
#define AUTH_FALLOS_LIBRES   3
#define AUTH_ESPERA_PASO_MS  500
#define AUTH_ESPERA_TOPE_MS  4000
#define AUTH_INACTIVIDAD_US  (300LL * 1000000LL)
#define AUTH_IP_MAX          4

/* Una entrada por cliente que ha fallado credenciales. El tipo va con NOMBRE
 * (y no anonimo) porque auth_ip_slot() lo devuelve. */
struct auth_ip_s {
    uint32_t ip;
    uint32_t fallos;
    int64_t  ultimo_us;
};
static struct auth_ip_s s_auth_ips[AUTH_IP_MAX];

/* Cierra la sesion de una peticion RECHAZADA (401/403/429).
 *
 * POR QUE CERRAR Y NO DRENAR EL CUERPO: el drenado que habia antes recorria
 * `req->content_len`, pero httpd_req_recv() NO lo decrementa -- lleva la cuenta
 * en un campo interno (`remaining_len`, ver httpd_txrx.c de la IDF) --, asi que
 * el bucle no terminaba nunca por esa condicion: se quedaba bloqueado hasta el
 * tope de 8 s (o hasta que el propio recv diera timeout) en CADA peticion
 * rechazada con cuerpo. Como el httpd es de UNA sola tarea, eso dejaba el
 * portal mudo 5-8 s por intento: justo lo que el drenado queria evitar. La
 * respuesta ya esta enviada cuando se llama a esto, y cerrar la sesion es lo
 * que el codigo hacia igualmente al final ("cliente lento"). Detectado en la
 * revision del 13-oct-2026. */
static void auth_cerrar_sesion(httpd_req_t *req, const char *motivo)
{
    ESP_LOGW(TAG, "%s: cierro la sesion", motivo);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
}

/* IP del cliente (0 si no se puede saber: entonces no se le aplica el freno por
 * IP, solo el global de siempre). */
static uint32_t auth_ip_cliente(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) != 0) return 0;
    return sa.sin_addr.s_addr;
}

/* Entrada de esa IP, creando una si hay hueco.
 *
 * OJO: la primera version de esto devolvia el hueco pero NUNCA escribia la IP en
 * la tabla, asi que todas las IPs acababan compartiendo la entrada 0 (el array
 * esta a cero y siempre parecia libre): el freno "por IP" era el contador global
 * de antes. Lo cazo la revision adversarial del 13-oct-2026. La IP se escribe
 * aqui, y tambien se CADUCA la entrada (si no, con el tiempo la tabla se llena
 * de IPs que ya no existen y una cabina nueva se queda sin hueco). */
static struct auth_ip_s *auth_ip_slot(uint32_t ip, int64_t ahora_us)
{
    if (ip == 0) return NULL;
    struct auth_ip_s *libre = NULL;
    for (int i = 0; i < AUTH_IP_MAX; i++) {
        if (s_auth_ips[i].ip == ip) return &s_auth_ips[i];
        bool caducada = (s_auth_ips[i].ip != 0) &&
                        (ahora_us - s_auth_ips[i].ultimo_us > AUTH_INACTIVIDAD_US);
        if ((s_auth_ips[i].ip == 0 || caducada) && !libre) libre = &s_auth_ips[i];
    }
    if (!libre) {
        /* Tabla llena: se recicla la entrada mas vieja, que es la que menos
         * probablemente este atacando ahora mismo. */
        int viejo = 0;
        for (int i = 1; i < AUTH_IP_MAX; i++)
            if (s_auth_ips[i].ultimo_us < s_auth_ips[viejo].ultimo_us) viejo = i;
        libre = &s_auth_ips[viejo];
    }
    libre->ip = ip;
    libre->fallos = 0;
    libre->ultimo_us = 0;
    return libre;
}

/* Comprobacion de credenciales CONTRA UNA CABECERA DADA, con las mismas
 * defensas para las dos (CSRF, freno por IP, log del 401 y cierre de sesion).
 * Las dos entradas publicas -- check_basic_auth y check_basic_auth_strict, que
 * siguen declaradas en config_server_internal.h -- son envoltorios de esto:
 * una compara contra la clave del portal y la otra contra la de administracion.
 *
 * POR QUE ESTA SEPARADO (revision adversarial del 13-oct-2026): cuando se
 * separaron las dos claves, el nivel abierto se dejo delegando en la funcion
 * estricta, asi que en cuanto se generaba la clave de administracion TODOS los
 * endpoints abiertos pasaban a pedirla: la app daba 401 en cada pantalla y el
 * satelite no podia mandar apuntes. */
static esp_err_t auth_comprueba(httpd_req_t *req, const char *esperada_cab,
                                const char *aviso, bool es_admin, bool prohibido)
{
    static uint32_t s_fallos = 0;
    static int64_t  s_ultimo_fallo_us = 0;

    /* CSRF (auditoria de seguridad del 13-oct-2026).
     *
     * El navegador manda la cabecera Basic GUARDADA sin preguntar, asi que una
     * pagina abierta en el movil del dueño (los datos moviles siguen activos
     * aunque el Wi-Fi no tenga internet) podia lanzar peticiones a la P4 -- GET
     * /ausente, POST /control, /settime -- y el navegador las firmaba con sus
     * credenciales: encender luces o la bomba, armar la vigilancia o cambiar la
     * hora sin que el usuario se enterara.
     *
     * Se exige que Origin (o, si no viene, Referer), CUANDO VIENE, sea de la
     * propia P4. Los clientes que no son navegadores (la app, el satelite,
     * curl) no mandan ninguno de los dos y siguen pasando igual: esto no
     * cambia el protocolo, solo cierra la puerta del navegador ajeno. */
    {
        char org[192] = {0}, ref[192] = {0}, host[64] = {0};
        esp_err_t eo = httpd_req_get_hdr_value_str(req, "Origin", org, sizeof(org));
        esp_err_t er = httpd_req_get_hdr_value_str(req, "Referer", ref, sizeof(ref));
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
        const char *cual = (eo == ESP_OK) ? org : ((er == ESP_OK) ? ref : NULL);
        bool fuera = false;
        if (cual) {
            /* SE COMPARA EL HOST, POR IGUALDAD (la primera version miraba
             * "contiene 192.168.4.1", y "http://192.168.4.1.malicioso.com"
             * tambien lo contiene: lo cazo la revision del 13-oct-2026). */
            const char *p = strstr(cual, "://");
            const char *h = p ? p + 3 : cual;
            char host_de[96] = {0};
            size_t n = 0;
            for (; h[n] && h[n] != '/' && h[n] != '?' && h[n] != '#' && n < sizeof(host_de) - 1; n++)
                host_de[n] = (h[n] >= 'A' && h[n] <= 'Z') ? (char)(h[n] + 32) : h[n];
            host_de[n] = 0;
            /* Sin puerto, que el navegador lo pone (o no) segun como se entre. */
            char *dp = strchr(host_de, ':');
            if (dp) *dp = 0;
            char host_req[64] = {0};
            size_t hn = 0;
            for (; host[hn] && host[hn] != ':' && hn < sizeof(host_req) - 1; hn++)
                host_req[hn] = (host[hn] >= 'A' && host[hn] <= 'Z') ? (char)(host[hn] + 32) : host[hn];
            host_req[hn] = 0;
            /* 'null' es un origen opaco (pagina local, sandbox): tampoco vale. */
            fuera = (host_de[0] == 0) || !strcmp(host_de, "null") ||
                    (host_req[0] && strcmp(host_de, host_req));
        } else {
            /* SIN Origin NI Referer. Aqui entra un <img referrerpolicy=no-referrer>
             * con las credenciales Basic cacheadas del navegador (los GET
             * /ausente y /settime TIENEN efecto), asi que se mira Sec-Fetch-Site:
             * los navegadores modernos lo mandan SIEMPRE y para una peticion
             * cruzada vale "cross-site". Los clientes que no son navegadores (la
             * app, el satelite, curl) no lo mandan y no les afecta. */
            char sfs[32] = {0};
            if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", sfs, sizeof(sfs)) == ESP_OK) {
                if (strcmp(sfs, "cross-site") == 0 || strcmp(sfs, "same-site") == 0) fuera = true;
            }
        }
        /* Cabecera DEMASIADO LARGA: httpd devuelve ESP_ERR_HTTPD_INVALID_REQ y la
         * damos por sospechosa (antes se trataba como ausente y pasaba). */
        if (eo == ESP_ERR_HTTPD_INVALID_REQ || er == ESP_ERR_HTTPD_INVALID_REQ) fuera = true;
        if (fuera) {
            ESP_LOGW(TAG, "peticion con Origin/Referer de fuera (%s): la rechazo "
                          "(posible CSRF)", cual ? cual : "(sin cabeceras)");
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Origen no permitido");
            /* Igual que en el 401: no se drena (no funcionaba) y se cierra. */
            auth_cerrar_sesion(req, "403 por origen");
            return ESP_FAIL;
        }
    }

    char auth[96] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                  auth, sizeof(auth));
    /* OJO: esta funcion es el nivel ESTRICTO (/ota, /keys, /save), asi que
     * compara contra la clave de ADMINISTRACION. El nivel abierto (la app, la
     * web, el satelite) NO pasa por aqui: tiene su propia comprobacion en
     * check_basic_auth(), contra s_auth_header.
     *
     * Y esto se arreglo el 13-oct-2026 despues de que la revision adversarial lo
     * cazara: al principio de separar las dos claves, el nivel abierto delegaba
     * en esta funcion, asi que en cuanto se generaba la clave de administracion
     * TODOS los endpoints abiertos pasaban a pedirla -- la app daba 401 en cada
     * pantalla y la cabina no podia mandar apuntes. Justo lo contrario de lo que
     * promete el boton. */
    const char *esperada = esperada_cab;
    if (err == ESP_OK && esperada && esperada[0] != '\0' &&
        auth_equal_ct(auth, esperada)) {
        s_fallos = 0;
        /* Tambien los de ESA IP: si no, seguiria contando fallos viejos y el
         * siguiente desliz la mandaria a 429 sin motivo. */
        struct auth_ip_s *ok_ip = auth_ip_slot(auth_ip_cliente(req), esp_timer_get_time());
        if (ok_ip) ok_ip->fallos = 0;
        ap_off_timer_kick();   /* peticion valida -> mantener el HTTP server vivo */
        return ESP_OK;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - s_ultimo_fallo_us > AUTH_INACTIVIDAD_US) s_fallos = 0;

    /* Freno POR IP, sin dormir la tarea del servidor. */
    uint32_t ip = auth_ip_cliente(req);
    if (ip != 0) {
        struct auth_ip_s *f = auth_ip_slot(ip, now_us);
        if (f) {
            if (now_us - f->ultimo_us > AUTH_INACTIVIDAD_US) f->fallos = 0;
            if (f->fallos > AUTH_FALLOS_LIBRES) {
                uint32_t espera_ms = (f->fallos - AUTH_FALLOS_LIBRES) * AUTH_ESPERA_PASO_MS;
                if (espera_ms > AUTH_ESPERA_TOPE_MS) espera_ms = AUTH_ESPERA_TOPE_MS;
                int64_t desde_ms = (now_us - f->ultimo_us) / 1000;
                if (desde_ms < (int64_t)espera_ms) {
                    char ra[12];
                    snprintf(ra, sizeof(ra), "%u",
                             (unsigned)((espera_ms - (uint32_t)desde_ms + 999) / 1000));
                    httpd_resp_set_status(req, "429 Too Many Requests");
                    httpd_resp_set_hdr(req, "Retry-After", ra);
                    httpd_resp_set_type(req, "text/plain");
                    httpd_resp_sendstr(req, "Demasiados intentos: espera un momento");
                    auth_cerrar_sesion(req, "429 por fuerza bruta");
                    return ESP_FAIL;
                }
            }
            f->fallos++;
            f->ultimo_us = now_us;
        }
    }
    s_ultimo_fallo_us = now_us;
    s_fallos++;

    /* Dejar rastro del 401: sin esto un cliente que no manda credenciales (la
     * app con usuario/clave sin configurar, p.ej.) es INVISIBLE en el log —
     * pasa lo mismo que si no hubiera pedido nada, y se diagnostica a ciegas.
     * NUNCA imprimir la cabecera recibida: log_capture la persistiria en la SD.
     * Throttle de 1 s porque la app repregunta en bucle y esto llenaria el log. */
    static int64_t last_warn_us = 0;
    if (now_us - last_warn_us > 1000000) {
        last_warn_us = now_us;
        ESP_LOGW(TAG, "401 en %s (fallos seguidos: %lu): %s", req->uri,
                 (unsigned long)s_fallos,
                 s_auth_header[0] == '\0' ? "portal cerrado (sin credenciales en NVS)"
                 : err != ESP_OK          ? "el cliente no manda cabecera Authorization"
                                          : "usuario o clave incorrectos");
    }
    /* 403 Y NO 401 CUANDO EL QUE PIDE YA ESTA DENTRO (auditoria del 14-oct-2026):
     * hay operaciones que un usuario autenticado en el nivel abierto NO puede
     * hacer (borrar un viaje). Con un 401, el navegador vuelve a pedir
     * credenciales que ya valen y la app las traduce a "faltan el usuario y la
     * clave del portal" -- el usuario acaba rellenando lo que ya tenia. Un 403
     * dice lo que pasa y el cuerpo llega tal cual. */
    httpd_resp_set_status(req, prohibido ? "403 Forbidden" : "401 Unauthorized");
    /* REALM PROPIO PARA EL NIVEL ESTRICTO: con el mismo, el navegador reenvia
     * las credenciales del portal (que ya tiene cacheadas) y el usuario ve un
     * "vuelve a pedir la clave" que no entiende. Con otro nombre, las pide
     * aparte. */
    if (!prohibido)
        httpd_resp_set_hdr(req, "WWW-Authenticate",
                           es_admin ? "Basic realm=\"Victron Display admin\""
                                    : "Basic realm=\"Victron Display\"");
    httpd_resp_set_type(req, "text/plain");
    /* El cuerpo se puede personalizar (aviso != NULL): hay sitios -- borrar un
     * viaje -- donde "Auth required" no le dice al usuario QUE credencial falta
     * ni donde se pone. Lo enseña la app (manda el cuerpo en el mensaje). */
    httpd_resp_sendstr(req, aviso && aviso[0] ? aviso : "Auth required");

    /* El cuerpo que quede por leer NO se drena: se cierra la sesion (ver el
     * comentario de auth_cerrar_sesion: el bucle de drenado no terminaba nunca
     * porque content_len no baja, y bloqueaba la unica tarea del httpd). */
    auth_cerrar_sesion(req, "401 sin credenciales validas");
    return ESP_FAIL;
}

/* NIVEL ESTRICTO: /ota (grabar firmware), /keys (claves AES de los Victron) y
 * /save. La clave de administracion, que si no esta puesta es la del portal. */
esp_err_t check_basic_auth_strict(httpd_req_t *req)
{
    return auth_comprueba(req, s_admin_header[0] ? s_admin_header : s_auth_header,
                          NULL, /*es_admin=*/true, /*prohibido=*/false);
}

/* Nivel estricto con un aviso propio en el 401.
 *
 * Existe por BORRAR UN VIAJE (auditoria del 14-oct-2026): es la unica operacion
 * de /api/viaje que destruye datos de la tarjeta y paso a pedir la clave de
 * administracion, pero el cliente que la usa (la app) manda la del portal: sin
 * un texto que lo explique, el usuario solo ve un 401. */
esp_err_t check_basic_auth_strict_aviso(httpd_req_t *req, const char *aviso)
{
    /* 403: el que pide ya esta autenticado en el nivel abierto, lo que le falta
     * es el permiso. Ver el comentario de auth_comprueba. */
    return auth_comprueba(req, s_admin_header[0] ? s_admin_header : s_auth_header,
                          aviso, /*es_admin=*/true, /*prohibido=*/true);
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
    /* NIVEL ABIERTO: la clave del PORTAL (la de la app y las pantallas), NO la
     * de administracion. Ver el comentario de check_basic_auth_strict(). */
    return auth_comprueba(req, s_auth_header, NULL, /*es_admin=*/false, /*prohibido=*/false);
#endif
}
