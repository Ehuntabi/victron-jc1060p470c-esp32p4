/* config_server_viaje.h — endpoint de los apuntes que manda el satelite 3.5".
 * Ver la cabecera del .c para el porque del canal HTTP y de las fases. */
#pragma once
#include "esp_http_server.h"

/* POST /api/viaje — Basic Auth ESTRICTA (escribe en la tarjeta). */
esp_err_t handle_api_viaje(httpd_req_t *req);

/* Arranca el timer que, MIENTRAS HAYA VIAJE ABIERTO, va dejando en su carpeta
 * una fila de telemetria cada 5 min y una de contadores cada hora. Llamar una
 * vez al iniciar; no hace nada mientras no haya viaje. */
void viaje_telemetria_start(void);

/* GET /data/viajes — lista de viajes guardados con su estado y su enlace. */
esp_err_t handle_data_viajes(httpd_req_t *req);
