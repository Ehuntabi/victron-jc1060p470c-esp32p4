#pragma once
/* Actualizacion del firmware por Wi-Fi. Ver ota_update.c. */
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GET /ota  -> pagina con el selector de fichero y la barra de progreso. */
esp_err_t ota_update_page(httpd_req_t *req);

/* POST /ota -> recibe el firmware EN CRUDO y lo instala. Contesta al navegador
 * y reinicia 1,5 s despues. */
esp_err_t ota_update_receive(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
