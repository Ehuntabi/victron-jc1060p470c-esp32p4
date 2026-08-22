/* config_server_viaje.h — endpoint de los apuntes que manda el satelite 3.5".
 * Ver la cabecera del .c para el porque del canal HTTP y de las fases. */
#pragma once
#include "esp_http_server.h"

/* POST /api/viaje — Basic Auth ESTRICTA (escribe en la tarjeta). */
esp_err_t handle_api_viaje(httpd_req_t *req);
