#pragma once
#include "esp_err.h"
#include "frigo.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DATALOGGER_MAX_ENTRIES  200

/* Formato del CSV del frigo, en UN solo sitio: lo escriben el volcado a la SD
 * (datalogger_flush), el CSV de RAM que sirve el portal (datalogger_get_csv),
 * la cabecera de relleno del portal cuando no hay datos y el simulador; y lo
 * leen el visor del firmware, la app y el analizador del PC.
 *
 * El 13-sep-2026 la columna min_solar_hoy se anadio solo a uno de los dos
 * escritores: los ficheros de la tarjeta -- que son el log de verdad -- se
 * quedaron sin el dato, y ademas con una cabecera de seis columnas para filas
 * de siete. Con la cabecera y la fila definidas aqui, o cambian las dos o no
 * cambia ninguna.
 *
 * La columna nueva va SIEMPRE al final: los lectores que van por indice
 * (log_browser exige nf >= 5 y usa fields[0..5]) siguen funcionando igual. */
#define DATALOGGER_CSV_HEADER \
    "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct,excedente_solar,min_solar_hoy\n"
#define DATALOGGER_CSV_ROW "%s,%s,%s,%s,%d,%d,%u\n"

typedef struct {
    char timestamp[24];
    float T_Aletas;
    float T_Congelador;
    float T_Exterior;
    uint8_t fan_percent;
    bool    excedente_solar;   /* 1 = frigo alimentado por excedente solar en esa muestra */
    uint16_t min_solar_hoy;    /* minutos acumulados hoy alimentado por solar (hasta esta muestra) */
} datalogger_entry_t;

esp_err_t datalogger_init(void);
esp_err_t datalogger_log(const frigo_state_t *frigo);
int datalogger_get_count(void);
const datalogger_entry_t *datalogger_get_entry(int index);
char *datalogger_get_csv(void);
void datalogger_flush(void);

/* Handle de la tarea de vuelco a SD. Solo para la medicion de
 * uxTaskGetStackHighWaterMark (ver stack_watch.c) -- nadie mas la necesita. */
TaskHandle_t datalogger_flush_task_handle(void);

/* Cierra los ficheros y DESMONTA la tarjeta para poder sacarla sin corromperla.
 * Despues de esto no se escribe mas hasta reiniciar. */
esp_err_t datalogger_close_sd(void);

/* true si la tarjeta sigue montada. */
bool datalogger_sd_montada(void);

/* Latido de watchdog para flush_task (mismo patron que
 * frigo_set_heartbeat_cb): este componente no puede incluir main/watchdog.h
 * directamente, asi que quien registra el callback (main.c) es quien conoce
 * wd_task_t. NULL = sin vigilar (comportamiento de siempre). */
typedef void (*datalogger_heartbeat_cb_t)(void);
void datalogger_set_heartbeat_cb(datalogger_heartbeat_cb_t cb);
