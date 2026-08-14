#pragma once
/* Handlers HTTP de graficos SVG (frigo/bateria historicos) para el portal
 * web. Extraido de config_server.c (2026-08-08) por tamano: registrado
 * desde config_server_start(). */

#include "esp_http_server.h"

esp_err_t handle_data_frigo(httpd_req_t *req);
esp_err_t handle_data_frigo_csv(httpd_req_t *req);
esp_err_t handle_data_ne185v_csv(httpd_req_t *req);
esp_err_t handle_data_bateria(httpd_req_t *req);
esp_err_t handle_data_bateria_csv(httpd_req_t *req);
esp_err_t handle_data_index(httpd_req_t *req);
