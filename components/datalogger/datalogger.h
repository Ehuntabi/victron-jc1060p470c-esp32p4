#pragma once
#include "esp_err.h"
#include "frigo.h"
#include <stdbool.h>

#define DATALOGGER_MAX_ENTRIES  200

typedef struct {
    char timestamp[24];
    float T_Aletas;
    float T_Congelador;
    float T_Exterior;
    uint8_t fan_percent;
} datalogger_entry_t;

esp_err_t datalogger_init(void);
esp_err_t datalogger_log(const frigo_state_t *frigo);
bool datalogger_is_ready(void);
int datalogger_get_count(void);
const datalogger_entry_t *datalogger_get_entry(int index);
char *datalogger_get_csv(void);
void datalogger_flush(void);
