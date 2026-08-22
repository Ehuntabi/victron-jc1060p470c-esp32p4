#pragma once
/* Handlers HTTP de descarga .tar (frigo/bateria/solar/capturas/vigilancia/
 * config/logs/viaje) para el portal web. Extraido de config_server.c
 * (2026-08-08) por tamano: registrado desde config_server_start(). */

#include "esp_http_server.h"

esp_err_t handle_data_frigo_tar(httpd_req_t *req);
esp_err_t handle_data_bateria_tar(httpd_req_t *req);
esp_err_t handle_data_solar_tar(httpd_req_t *req);
/* UN viaje: /data/viaje.tar?v=<carpeta>. Sin 'v' redirige a la lista. */
esp_err_t handle_data_viaje_tar(httpd_req_t *req);
/* El historico entero (bateria+solar+frigo). Se llamaba viaje.tar y mentia. */
esp_err_t handle_data_historico_tar(httpd_req_t *req);
esp_err_t handle_data_capturas_tar(httpd_req_t *req);
esp_err_t handle_data_vigilancia_tar(httpd_req_t *req);
esp_err_t handle_data_config_tar(httpd_req_t *req);
esp_err_t handle_data_logs_tar(httpd_req_t *req);
