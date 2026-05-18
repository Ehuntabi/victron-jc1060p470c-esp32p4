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

/* Snapshot atómico del estado interno, para consumidores que no quieren
 * parsear JSON (ej: publisher ESP-NOW). Solo campos cacheados aquí; otros
 * módulos (pzem, ne185, frigo, trip) exponen sus propios getters. */
typedef struct {
    bool     bat_has;
    uint16_t soc_deci;
    uint16_t bat_v_centi;
    int32_t  bat_i_milli;

    bool     dcdc_has;
    uint16_t dc_in_v_centi;
    uint16_t dc_out_v_centi;
    uint8_t  dc_state;
} dashboard_snapshot_t;

void dashboard_state_snapshot(dashboard_snapshot_t *out);

#ifdef __cplusplus
}
#endif
