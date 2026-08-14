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
void mount_spiffs(void);
esp_err_t serve_from_spiffs(httpd_req_t *req, const char *uri);
