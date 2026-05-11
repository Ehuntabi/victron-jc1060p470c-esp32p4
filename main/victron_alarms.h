#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Devuelve un string corto en español (max ~14 chars) describiendo la
 * primera alarma activa de la bitmask. NULL si no hay alarma. */
const char *victron_alarm_reason_string(uint16_t mask);

/* Devuelve un string corto en español para un charger_error. NULL si == 0. */
const char *victron_charger_error_string(uint8_t code);

/* VE.Bus error code (uint8) -> string corto. NULL si == 0. */
const char *victron_vebus_error_string(uint8_t code);

#ifdef __cplusplus
}
#endif
