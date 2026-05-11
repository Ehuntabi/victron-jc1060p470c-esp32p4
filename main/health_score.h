#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEALTH_OK    = 0,   /* verde */
    HEALTH_WARN  = 1,   /* ambar */
    HEALTH_ALARM = 2,   /* rojo */
} health_level_t;

/* Evalua el estado global combinando BLE timeout, alarmas Victron, SoC,
 * freezer, PZEM, watchdog resets, etc. Si `reason_out` no es NULL escribe
 * un string corto con el motivo dominante (max ~24 chars). */
health_level_t health_score_evaluate(char *reason_out, size_t maxlen);

#ifdef __cplusplus
}
#endif
