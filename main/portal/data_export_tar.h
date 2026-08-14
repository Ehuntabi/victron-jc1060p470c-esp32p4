#pragma once
/* Handlers HTTP de descarga .tar (frigo/bateria/solar/capturas/vigilancia/
 * config/logs/viaje) para el portal web. Extraido de config_server.c
 * (2026-08-08) por tamano: registrado desde config_server_start(). */

#include "esp_http_server.h"

esp_err_t handle_data_frigo_tar(httpd_req_t *req);
esp_err_t handle_data_bateria_tar(httpd_req_t *req);
esp_err_t handle_data_solar_tar(httpd_req_t *req);
esp_err_t handle_data_viaje_tar(httpd_req_t *req);
esp_err_t handle_data_capturas_tar(httpd_req_t *req);
esp_err_t handle_data_vigilancia_tar(httpd_req_t *req);
esp_err_t handle_data_config_tar(httpd_req_t *req);
esp_err_t handle_data_logs_tar(httpd_req_t *req);
