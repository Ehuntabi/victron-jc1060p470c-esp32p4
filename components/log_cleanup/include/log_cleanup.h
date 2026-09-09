#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa el modulo de limpieza de logs.
   max_days_keep: ficheros con antiguedad > max_days_keep seran borrados.
   Despues del init, programa una tarea diaria automatica.
   Tambien hace un primer barrido pasados 5s. */
void log_cleanup_init(int max_days_keep);

/* Devuelve cuantos ficheros estan a punto de ser borrados (con antiguedad >= max_days - 1).
   Usar para detectar si hay que mostrar aviso al arrancar. */
int log_cleanup_files_pending_warning(int max_days_keep);

/* Hace un barrido inmediato. Retorna numero de ficheros borrados. */
int log_cleanup_run_now(int max_days_keep);

/* Handle de la tarea de barrido. Solo para la medicion de
 * uxTaskGetStackHighWaterMark (ver stack_watch.c) -- nadie mas la necesita. */
TaskHandle_t log_cleanup_task_handle(void);

/* Latido de watchdog para cleanup_task (mismo patron que
 * frigo_set_heartbeat_cb/datalogger_set_heartbeat_cb/
 * battery_history_set_heartbeat_cb): este componente no puede incluir
 * main/watchdog.h directamente, asi que quien registra el callback
 * (main.c) es quien conoce wd_task_t. NULL = sin vigilar. */
typedef void (*log_cleanup_heartbeat_cb_t)(void);
void log_cleanup_set_heartbeat_cb(log_cleanup_heartbeat_cb_t cb);

#ifdef __cplusplus
}
#endif
