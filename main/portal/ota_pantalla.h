#pragma once
/* OTA de las DOS pantallas (la de 5" y la de 3,5") desde la P4.
 *
 * POR QUE EXISTE: la P4 lleva la tarjeta SD y el portal Wi-Fi, y las cabinas
 * solo tienen un enlace HTTP contra ella. Hasta ahora, actualizar una cabina
 * obligaba a sacarla del sitio y grabarla por cable. Con esto el dueno sube el
 * .bin una vez a la P4 (por el portal, desde el movil o el PC) y cada cabina se
 * lo baja y lo instala sola.
 *
 * CONTRATO (lo usa el cliente que corre en las cabinas):
 *   GET  /api/pantalla/info                  puerto 80    nivel abierto
 *   GET  /api/pantalla/bin?modelo=<m>        puerto 8081  nivel abierto
 *   POST /api/pantalla/subir?modelo=<m>      puerto 8081  nivel ADMINISTRACION
 *
 *   Modelos validos: "cabina5" y "35cabina". Los ficheros viven en
 *   /sdcard/firmware/cabina5.bin y /sdcard/firmware/35cabina.bin.
 *
 * El cliente NO esta en este repo: cualquier cambio de nombres, campos o
 * codigos de estado hay que hablarlo con el repo de las cabinas. El contrato
 * esta fijado en el informe de la tarea y en los comentarios de ota_pantalla.c.
 */
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GET /api/pantalla/info: version, tamano y sha256 de cada modelo. Los modelos
 * sin fichero (o con un fichero que no es un firmware valido) salen a null. */
esp_err_t ota_pantalla_info(httpd_req_t *req);

/* GET /api/pantalla/bin?modelo=...: el .bin ENTERO con su Content-Length. La
 * cabina lo necesita para pasarselo a esp_ota_begin sin adivinar el tamano. */
esp_err_t ota_pantalla_bin(httpd_req_t *req);

/* POST /api/pantalla/subir?modelo=...: cuerpo = el .bin. Escribe a un .tmp,
 * valida la cabecera y solo entonces lo publica con el nombre bueno. */
esp_err_t ota_pantalla_subir(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
