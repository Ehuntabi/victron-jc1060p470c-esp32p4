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

    /* La alarma del congelador no se duplica en la barra inferior:
     * el propio overview parpadea la temperatura T_Congelador en rojo y
     * dispara la alarma sonora (con mute al pulsar). */

out:
    if (reason_out && maxlen > 0) {
        strncpy(reason_out, reason, maxlen - 1);
        reason_out[maxlen - 1] = 0;
    }
    (void)s_last_ble_ts_us;
    return level;
}
