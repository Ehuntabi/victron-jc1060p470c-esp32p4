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

/* Definida en config_server.c (ciclo de vida del AP/portal). La necesita
 * check_basic_auth[_strict] para mantener vivo el HTTP server mientras haya
 * peticiones validas. */
void ap_off_timer_kick(void);

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
