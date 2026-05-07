#pragma once
#include <stdbool.h>
#include <stddef.h>

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

#ifdef __cplusplus
}
#endif
