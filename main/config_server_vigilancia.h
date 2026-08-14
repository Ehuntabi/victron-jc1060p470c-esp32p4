#pragma once
/* Handlers HTTP de la galeria de vigilancia (snapshot + capturas de
 * movimiento) para el portal web. Extraido de config_server.c (2026-08-14)
 * por tamano: registrado desde config_server_start(). */

#include "esp_http_server.h"

esp_err_t handle_snapshot(httpd_req_t *req);
esp_err_t handle_vigilancia(httpd_req_t *req);
