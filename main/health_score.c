#include "health_score.h"

#include <string.h>
#include <stdio.h>
#include "dashboard_state.h"
#include "alerts.h"
#include "ui.h"   /* ui_get_freezer_alarm(): verdad robusta de main.c */

/* Semaforo de salud de la barra inferior. Combina los estados que NO tienen
 * ya su propio indicador en la barra:
 *   - SoC de bateria (umbrales configurables en alerts)
 *   - Alarma del congelador (criterio robusto calculado en main.c)
 * El estado BLE tiene su propio icono Bluetooth en la barra, asi que no se
 * duplica aqui. */
health_level_t health_score_evaluate(char *reason_out, size_t maxlen)
{
    health_level_t level = HEALTH_OK;
    char reason[28] = "OK";

    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);
    /* Truncamiento /10 para coincidir con la logica de alarma SoC de ui.c. */
    int soc = snap.bat_has ? snap.soc_deci / 10 : -1;

    /* --- ALARMAS (rojo) --- */
    if (soc >= 0 && soc <= alerts_get_soc_critical()) {
        snprintf(reason, sizeof reason, "Bateria %d%%", soc);
        level = HEALTH_ALARM;
    } else if (ui_get_freezer_alarm()) {
        strcpy(reason, "Congelador");
        level = HEALTH_ALARM;
    /* --- AVISOS (ambar) --- */
    } else if (soc >= 0 && soc <= alerts_get_soc_warning()) {
        snprintf(reason, sizeof reason, "Bateria %d%%", soc);
        level = HEALTH_WARN;
    }

    if (reason_out && maxlen > 0) {
        strncpy(reason_out, reason, maxlen - 1);
        reason_out[maxlen - 1] = 0;
    }
    return level;
}
