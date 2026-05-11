#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "victron_records.h"
#include "victron_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Llamar en el hook ui_on_panel_data: cachea los campos relevantes del
 * dispositivo Victron en una struct atómica que el servidor HTTP puede leer. */
void dashboard_state_on_record(const victron_data_t *data);

/* Serializa el snapshot actual a JSON. Devuelve los bytes escritos (sin NUL).
 * Si maxlen es insuficiente, devuelve 0 y deja buf vacío. */
size_t dashboard_state_to_json(char *buf, size_t maxlen);

#ifdef __cplusplus
}
#endif
