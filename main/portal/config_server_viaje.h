/* config_server_viaje.h — endpoint de los apuntes que manda el satelite 3.5".
 * Ver la cabecera del .c para el porque del canal HTTP y de las fases. */
#pragma once
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* POST /api/viaje — Basic Auth ESTRICTA (escribe en la tarjeta). */
esp_err_t handle_api_viaje(httpd_req_t *req);

/* Arranca el timer que, MIENTRAS HAYA VIAJE ABIERTO, va dejando en su carpeta
 * una fila de telemetria cada 5 min y una de contadores cada hora. Llamar una
 * vez al iniciar; no hace nada mientras no haya viaje. */
void viaje_telemetria_start(void);

/* Handle de la tarea de telemetria. Solo para la medicion de
 * uxTaskGetStackHighWaterMark (ver stack_watch.c) -- nadie mas la necesita. */
TaskHandle_t viaje_tick_task_handle(void);

/* Nombre de la carpeta del viaje ABIERTO, o false si no hay ninguno.
 *
 * Lo usan los logs (main/log_capture/log_capture.c) para dejar escrito A QUE
 * VIAJE pertenecen: sin eso, dos viajes distintos acaban con ficheros del mismo
 * nombre y no hay forma de saber de cual es cada uno. */
bool viaje_abierto_nombre(char *out, size_t n);

/* Reparacion del estado del viaje al arrancar, CON LA SD YA MONTADA.
 *
 * Si la NVS dice que hay un viaje abierto y su carpeta no esta en la tarjeta,
 * limpia el estado (no cierra nada: sus datos no estan) para que la P4 no se
 * quede bloqueada sin poder cerrar ni abrir viajes. Ver el comentario largo en
 * config_server_viaje.c. Se llama desde app_main() una sola vez. */
void viaje_reparar_al_arrancar(void);

/* GET /data/viajes — lista de viajes guardados con su estado y su enlace. */
esp_err_t handle_data_viajes(httpd_req_t *req);
