#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t reset_epoch;      /* segundos desde epoch; 0 si nunca */
    double wh_charged;        /* energia entrante acumulada */
    double wh_discharged;     /* energia saliente acumulada */
    double ah_charged;        /* carga entrante (A*h) */
    double ah_discharged;     /* carga saliente (A*h) */
    int64_t seconds_running;  /* tiempo "activo" (con sample en intervalo) */
} trip_computer_t;

/* Carga el estado persistido en NVS (o ceros si no hay). Llamar al boot. */
void trip_computer_init(void);

/* Hook: integra energia/carga entre samples reales del BMV. */
void trip_computer_on_battery(int32_t i_milli, uint16_t v_centi);

/* Reset manual de todos los contadores. Guarda inmediatamente en NVS. */
void trip_computer_reset(void);

/* Copia el snapshot actual a out. Thread-safe (mutex interno). */
void trip_computer_get(trip_computer_t *out);

#ifdef __cplusplus
}
#endif
