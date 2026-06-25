#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* GPIOs físicamente accesibles vía el header JP1 (2x13 / 2.54 mm) del módulo
 * Guition JC1060P470C_I. Antes estaban en 26 y 21, que NO están enrutados al
 * conector — sólo eran selecciones sobre el papel. */
#define FRIGO_ONEWIRE_GPIO    4    /* JP1 pin 13 — DS18B20 1-Wire (pullup 4.7 kΩ a 3.3 V) */
#define FRIGO_FAN_GPIO        5    /* JP1 pin 15 — PWM ventilador (LEDC) */
#define FRIGO_FAN_FREQ_HZ  25000
#define FRIGO_MAX_SENSORS     3

typedef enum {
    FRIGO_SLOT_ALETAS     = 0,
    FRIGO_SLOT_CONGELADOR = 1,
    FRIGO_SLOT_EXTERIOR   = 2,
} frigo_slot_t;

/* Modo de funcionamiento del ventilador.
 * AUTO: pct se calcula segun T_Aletas y los thresholds T_min/T_max.
 * OFF/MODE_50/MODE_100: pct fijo, los thresholds se ignoran.
 * No persiste en NVS: cada reset arranca en AUTO. */
typedef enum {
    FRIGO_MODE_AUTO = 0,
    FRIGO_MODE_OFF  = 1,
    FRIGO_MODE_50   = 2,
    FRIGO_MODE_100  = 3,
} frigo_mode_t;

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
    frigo_mode_t mode;
} frigo_state_t;

typedef void (*frigo_update_cb_t)(const frigo_state_t *state);

esp_err_t frigo_init(frigo_update_cb_t cb);

/* Copia atomica del estado bajo mutex. Sustituye al antiguo frigo_get_state()
 * que devolvia un puntero al estado interno: los lectores (UI, udp_tx) podian
 * leer campos a medio actualizar por frigo_task/sim (datos rasgados entre
 * campos). Copiar bajo lock garantiza una foto coherente. */
void frigo_get_state_copy(frigo_state_t *out);

/* Hook de "estoy vivo": frigo_task lo invoca en cada iteracion de su bucle.
 * Permite a la app vigilar la tarea (watchdog) sin que el componente frigo
 * dependa de modulos de la app. NULL = sin vigilancia. */
typedef void (*frigo_heartbeat_cb_t)(void);
void frigo_set_heartbeat_cb(frigo_heartbeat_cb_t cb);

/* Inyeccion para modo simulacion: sobreescribe temperaturas y % fan. */
void frigo_sim_inject(float t_aletas, float t_congelador,
                      float t_exterior, uint8_t fan_percent);
esp_err_t frigo_set_assignment(frigo_slot_t slot, uint8_t sensor_idx);
esp_err_t frigo_set_thresholds(uint8_t t_min, uint8_t t_max);
void frigo_set_mode(frigo_mode_t m);
void frigo_addr_to_str(const frigo_sensor_addr_t *addr, char *buf, size_t len);
