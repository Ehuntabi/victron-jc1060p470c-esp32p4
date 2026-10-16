/* config_server_viaje.h — endpoint de los apuntes que manda el satelite 3.5".
 * Ver la cabecera del .c para el porque del canal HTTP y de las fases. */
#pragma once
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* POST /api/viaje — Basic Auth del PORTAL (nivel abierto: es el canal de las
 * cabinas y solo tienen esas credenciales). */
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
 * nombre y no hay forma de saber de cual es cada uno.
 *
 * Y lo usa el .tar (data_export_tar.c) a traves de viaje_es_el_abierto(). */
bool viaje_abierto_nombre(char *out, size_t n);

/* ¿'nombre' (el nombre de la carpeta, sin ruta) es el viaje que la P4 tiene
 * ABIERTO ahora mismo? (16-oct-2026)
 *
 * Existe porque un cierre que falla puede dejar resumen.txt escrito con el viaje
 * todavia abierto en la NVS (ver escribir_resumen(): se escribe a .tmp y se
 * renombra, que cierra el caso del resumen a medias, pero entre el rename y el
 * commit de la NVS el viaje sigue abierto). Sin esto, /data/viajes lo enseñaba
 * como "Listo" y se podia descargar: el usuario se bajaba un viaje que la P4
 * cree que sigue en curso, y la cabina le seguia mandando apuntes. Con esto, el
 * .tar lo rechaza con 409 y lo dice. */
bool viaje_es_el_abierto(const char *nombre);

/* Vuelca a la SD lo unico que vive en RAM y se perderia en un reinicio: los
 * puntos de ruta del viaje abierto (hasta 5 min de traza). Se llama desde TODOS
 * los caminos de reinicio menos el del watchdog, que no puede hacer I/O de
 * tarjeta porque viene de un cuelgue. Ver el comentario del .c. */
void viaje_flush_antes_de_reiniciar(void);

/* Deja anotado en el diario del viaje abierto que el reloj ha dado un salto
 * (lo llama gps.c cuando el GPS corrige la hora mas de 5 min). */
void viaje_nota_hora_corregida(int64_t delta_s);

/* Cuantos fallos de escritura ha tenido el viaje (apuntes, cierre, telemetria,
 * contadores, ruta). Lo mira el icono de la SD de la barra: antes solo reflejaba
 * los fallos del datalogger, asi que podia estar verde con el viaje sin
 * escribirse (auditoria del 13-oct-2026, hallazgo 14). */
uint32_t viaje_fallos_escritura(void);

/* Hueco libre y total de la tarjeta, medidos por el modulo del viaje (el tick de
 * telemetria los refresca cada 5 min). false si no se ha podido medir. Lo pinta
 * Ajustes -> About, que es donde se mira cuando algo no se puede escribir. */
bool viaje_sd_hueco(uint64_t *libre_out, uint64_t *total_out);

/* Fallos de NVS del viaje (no de la tarjeta): van aparte porque el icono de la
 * SD es de la SD. Se enseñan junto a la ocupacion de la NVS en Ajustes. */
uint32_t viaje_fallos_nvs(void);

/* Vuelca la ruta del viaje abierto YA, dejando el motivo escrito en el diario.
 * Lo usan los reinicios (viaje_flush_antes_de_reiniciar) y los avisos graves,
 * como la alarma de bateria baja: al corte brusco se iban hasta 5 min de traza
 * que estaban en RAM. */
/* false = no se volco: no hay viaje abierto (lo mas comun), no se pudo coger
 * el cerrojo de la SD o la tarjeta no quiso. */
bool viaje_volcar_ahora(const char *motivo);

/* Pide un volcado SIN hacerlo: lo ejecuta la tarea del viaje en su siguiente
 * vuelta (1 s). Para quien no puede tocar la SD -- el evaluador de alarmas corre
 * en un lv_timer, o sea en la tarea de LVGL, y ahi un cerrojo de la SD de hasta
 * 3 s congela la pantalla. 'motivo': 0 = bateria baja, otro = volcado a mano. */
void viaje_pedir_volcado(int motivo);

/* Reparacion del estado del viaje al arrancar, CON LA SD YA MONTADA.
 *
 * Si la NVS dice que hay un viaje abierto y su carpeta no esta en la tarjeta,
 * limpia el estado (no cierra nada: sus datos no estan) para que la P4 no se
 * quede bloqueada sin poder cerrar ni abrir viajes. Ver el comentario largo en
 * config_server_viaje.c. Se llama desde app_main() una sola vez. */
void viaje_reparar_al_arrancar(void);

/* GET /data/viajes — lista de viajes guardados con su estado y su enlace. */
esp_err_t handle_data_viajes(httpd_req_t *req);

/* Borra una carpeta de viaje de la tarjeta (el nucleo de op_borrar, sin HTTP).
 * Devuelve 0 si se borro, o un codigo HTTP con el motivo en 'motivo'. La usa la
 * papelera de la web (config_server.c) ademas de la API de la app. */
int viaje_borrar_carpeta(const char *nombre, char *motivo, size_t n);
