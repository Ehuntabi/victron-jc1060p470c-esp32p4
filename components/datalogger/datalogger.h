#pragma once
#include "esp_err.h"
#include "frigo.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DATALOGGER_MAX_ENTRIES  200

typedef struct {
    char timestamp[24];
    float T_Aletas;
    float T_Congelador;
    float T_Exterior;
    uint8_t fan_percent;
    bool    excedente_solar;   /* 1 = frigo alimentado por excedente solar en esa muestra */
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
