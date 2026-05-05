#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define FRIGO_ONEWIRE_GPIO   26
#define FRIGO_FAN_GPIO        9
#define FRIGO_FAN_FREQ_HZ  25000
#define FRIGO_MAX_SENSORS     3

typedef enum {
    FRIGO_SLOT_ALETAS     = 0,
    FRIGO_SLOT_CONGELADOR = 1,
    FRIGO_SLOT_EXTERIOR   = 2,
} frigo_slot_t;

typedef struct {
    uint64_t address;
    bool     valid;
} frigo_sensor_addr_t;

typedef struct {
    float   T_Aletas;
    float   T_Congelador;
    float   T_Exterior;
    uint8_t fan_percent;
    uint8_t T_min;
    uint8_t T_max;
    uint8_t n_sensors;
    frigo_sensor_addr_t sensors[FRIGO_MAX_SENSORS];
    uint8_t assignment[FRIGO_MAX_SENSORS];
} frigo_state_t;

typedef void (*frigo_update_cb_t)(const frigo_state_t *state);

esp_err_t frigo_init(frigo_update_cb_t cb);
const frigo_state_t *frigo_get_state(void);
esp_err_t frigo_set_assignment(frigo_slot_t slot, uint8_t sensor_idx);
esp_err_t frigo_set_thresholds(uint8_t t_min, uint8_t t_max);
void frigo_addr_to_str(const frigo_sensor_addr_t *addr, char *buf, size_t len);
