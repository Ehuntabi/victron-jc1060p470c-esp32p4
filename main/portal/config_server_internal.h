#pragma once
/* Compartido SOLO entre los ficheros que implementan el portal HTTP
 * (config_server.c, charts_svg.c, data_export_tar.c): no es una API publica
 * del componente main, es la costura interna para poder partir
 * config_server.c en varios .c sin duplicar la logica de auth. */

#include "esp_http_server.h"

/* Definidas en config_server_auth.c. */
esp_err_t check_basic_auth(httpd_req_t *req);
esp_err_t check_basic_auth_strict(httpd_req_t *req);
extern const char SETTIME_SCRIPT[];

/* Definida en config_server_ap.c (ciclo de vida del AP/portal). La necesita
 * check_basic_auth[_strict] para mantener vivo el HTTP server mientras haya
 * peticiones validas. */
void ap_off_timer_kick(void);

/* Definidas en config_server.c (dueño real de s_httpd/s_dns: es quien hace
 * httpd_start/start_dns_server). Las necesita config_server_ap.c para
 * parar el portal/DNS desde el ciclo de vida del AP sin tocar esos
 * estaticos directamente. */
void cfg_http_stop(void);
void cfg_dns_stop(void);

/* Segunda instancia httpd, SOLO para lo que puede tardar segundos y dejar
 * el server principal mudo mientras dura (.tar completos, OTA, galeria de
 * vigilancia con JPEGs completos) -- esp_http_server es de una sola tarea,
 * asi que una peticion larga bloquea TODO lo demas (incluido /api/state,
 * que la app sondea a 1 Hz) hasta que termina. El puerto 8081 es fijo,
 * como ya lo es la IP del AP (192.168.4.1, ver el resto del portal): un
 * solo camino de acceso conocido, sin necesidad de resolver el Host de
 * cada peticion en tiempo real.
 *
 * Cualquier href generado hacia uno de esos endpoints tiene que llevar
 * PORTAL_HEAVY_BASE por delante (charts_svg.c, config_server_viaje.c,
 * config_server_vigilancia.c) -- si no, el navegador los pide al puerto
 * 80 de siempre, donde ya no existen. Detectado por el usuario el
 * 09-sep-2026. */
#define PORTAL_HEAVY_PORT 8081
#define PORTAL_HEAVY_BASE "http://192.168.4.1:8081"

/* Helper macro: pone al inicio de los handlers que exigen auth. Si falla
 * la respuesta 401 ya está enviada — devolvemos ESP_OK para que el http
 * server no reintente ni loggee error. NO aplicar al captive-portal
 * redirect (handle_captive_redirect): rompería la detección de portal. */
#define REQUIRE_AUTH(req) do { \
    if (check_basic_auth(req) != ESP_OK) return ESP_OK; \
} while (0)

/* Igual, pero exigiendo credenciales SIEMPRE. Solo para lo que puede dejar la
 * pantalla inservible o entregar las claves AES de los Victron. */
#define REQUIRE_AUTH_STRICT(req) do { \
    if (check_basic_auth_strict(req) != ESP_OK) return ESP_OK; \
} while (0)

/* Escape HTML de texto libre (nombres de fichero/carpeta que vienen de la SD,
 * o el destino de viaje que se teclea en la 3,5"): sustituye & < > " ' por sus
 * entidades para que no rompan el HTML ni los atributos (XSS almacenado si el
 * nombre llegara a contener algo ejecutable). Antes era static en
 * config_server_viaje.c; se sube aqui para usarla tambien en la galeria de
 * vigilancia (config_server_vigilancia.c) sin duplicarla. out debe ser al
 * menos 4x mayor que in por si cada caracter se convierte en la entidad mas
 * larga (&amp;). */
static inline void html_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 6 < out_len; i++) {
        switch (in[i]) {
            case '&':  memcpy(out + o, "&amp;",  5); o += 5; break;
            case '<':  memcpy(out + o, "&lt;",   4); o += 4; break;
            case '>':  memcpy(out + o, "&gt;",   4); o += 4; break;
            case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(out + o, "&#39;",  5); o += 5; break;
            default:   out[o++] = in[i]; break;
        }
    }
    out[o] = '\0';
}
