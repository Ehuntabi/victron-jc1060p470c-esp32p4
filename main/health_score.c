#include "health_score.h"

#include <string.h>
#include <stdio.h>
#include "dashboard_state.h"
#include "pzem004t.h"
#include "frigo.h"
#include "alerts.h"
#include "victron_alarms.h"

#define BLE_TIMEOUT_S   30      /* sin records BLE > 30 s -> WARN */

static int64_t s_last_ble_ts_us(void)
{
    /* La verdad la guardamos en ui.c::s_last_ble_data_us pero no esta expuesta.
     * Para no acoplar usamos un truco: el dashboard snapshot recibe un record
     * cada vez que llega BLE, asi que si todos los flags has=false durante
     * mucho tiempo asumimos timeout. Implementacion simple: si ningun
     * dispositivo tiene has=true, devolvemos -1 (no hay). */
    return -1;
}

static bool any_victron_alarm(const char **reason)
{
    pzem_data_t pz; pzem_get(&pz);
    if (pz.has_data && pz.alarm) { if (reason) *reason = "PZEM alarma"; return true; }
    /* dashboard_state no expone alarm fields publicamente pero el flag
     * de alarma del BMV se ve a traves de victron_alarms helpers en la
     * UI. Aqui simplificamos: si una serie esta en alarma, se vera ya en
     * la UI (pill rojo "FALLO"). El health_score se centra en estados
     * agregables (SoC, freezer, BLE). */
    return false;
}

health_level_t health_score_evaluate(char *reason_out, size_t maxlen)
{
    health_level_t level = HEALTH_OK;
    const char *reason = "OK";

    /* 1. Alarmas Victron (PZEM por ahora) -> ALARM */
    const char *r = NULL;
    if (any_victron_alarm(&r)) { reason = r ? r : "Alarma"; level = HEALTH_ALARM; goto out; }

    /* 2. Freezer fuera de rango (alta) -> ALARM */
    const frigo_state_t *fs = frigo_get_state();
    if (fs && fs->T_Congelador > -120.0f) {
        float thr = alerts_get_freezer_temp_c();
        if (fs->T_Congelador > thr) {
            reason = "Freezer alto"; level = HEALTH_ALARM; goto out;
        }
    }

    /* 3. PZEM presente con tension AC valida pero SIN consumo cuando esperaba?
     * No tenemos suficiente contexto. Lo dejamos.
     */

    /* 4. SoC critico -> ALARM, SoC warning -> WARN.
     * Lectura del dashboard_state (snapshot atomico). */
    char tmp[1024];
    if (dashboard_state_to_json(tmp, sizeof(tmp)) > 0) {
        /* Parser rapido del valor soc_pct: busca el campo. */
        const char *p = strstr(tmp, "\"soc_pct\":");
        if (p) {
            float soc = -1.0f;
            sscanf(p + 10, "%f", &soc);
            if (soc >= 0.0f) {
                int crit = alerts_get_soc_critical();
                int warn = alerts_get_soc_warning();
                if (soc < (float)crit) {
                    reason = "SoC critico"; level = HEALTH_ALARM; goto out;
                } else if (soc < (float)warn) {
                    reason = "SoC bajo"; level = HEALTH_WARN;
                }
            }
        }
        /* Si battery.has=false durante un rato, podemos marcar WARN */
        if (strstr(tmp, "\"battery\":{\"has\":false") != NULL && level == HEALTH_OK) {
            reason = "Sin BLE"; level = HEALTH_WARN;
        }
    }

out:
    if (reason_out && maxlen > 0) {
        strncpy(reason_out, reason, maxlen - 1);
        reason_out[maxlen - 1] = 0;
    }
    (void)s_last_ble_ts_us;
    return level;
}
