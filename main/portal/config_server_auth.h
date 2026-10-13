#pragma once
/* Auth del portal (Basic Auth) + montaje/servido de SPIFFS para el portal
 * web. Extraido de config_server.c (2026-08-14) por tamano: registrado desde
 * config_server_start(). check_basic_auth/check_basic_auth_strict siguen
 * declaradas en config_server_internal.h (las usan charts_svg.c y
 * data_export_tar.c via REQUIRE_AUTH); aqui solo lo que llama
 * config_server_start() directamente. */

#include "esp_http_server.h"
#include <stddef.h>

void http_auth_init(void);

/* Cambia usuario y clave del portal (Ajustes -> Wi-Fi). Devuelve ESP_OK si han
 * quedado guardadas y en vigor; ESP_ERR_INVALID_ARG si son vacias, demasiado
 * largas o llevan caracteres que rompen el Basic Auth. */
esp_err_t config_server_set_web_credentials(const char *user, const char *pass);

/* Credenciales de ADMINISTRACION: las que piden /ota, /keys y /save, aparte de
 * las del resto del portal (auditoria de seguridad del 13-oct-2026: la misma
 * clave para leer el estado y para reescribir el firmware es dar acceso total a
 * quien tenga la de leer). Si no hay ninguna puesta, el nivel estricto usa las
 * de siempre, para no dejar a nadie fuera. */
void config_server_get_admin_credentials(char *user, size_t ulen,
                                         char *pass, size_t plen);
esp_err_t config_server_set_admin_credentials(const char *user, const char *pass);
void mount_spiffs(void);
esp_err_t serve_from_spiffs(httpd_req_t *req, const char *uri);
