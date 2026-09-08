/* ble_ingest.c - ver ble_ingest.h */
#include "ble_ingest.h"
#include "ui.h"
#include "battery_history.h"
#include "alerts.h"
#include "data/solar_daily.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdint.h>

/* Altura fija (mA) del pulso on/off del Orion Tr en el log de bateria: el Orion
 * Tr (0x04) NO reporta corriente, solo estado, asi que dibujamos esta altura
 * mientras carga y 0 cuando no. Sirve para ver CUANDO y CUANTO tiempo carga; NO
 * es corriente real (ajustable). */
#define ORION_TR_ON_MILLIAMPS  10000   /* 10 A */

void ble_ingest_feed_history(const victron_data_t *d)
{
    switch (d->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            battery_history_update_latest(BH_SRC_BATTERY_MONITOR,
                d->record.battery.battery_current_milli,
                d->record.battery.battery_voltage_centi);
            /* Deteccion cruce de SoC */
            uint16_t soc_dp = d->record.battery.soc_deci_percent;
            if (soc_dp != 0xFFFF) {
                int soc_pct = soc_dp / 10;
                int crit_th = alerts_get_soc_critical();
                int warn_th = alerts_get_soc_warning();
                static int s_last_soc = -1;
                static int64_t s_last_soc_us = 0;
                static bool s_crit_active = false;
                static bool s_warn_active = false;
                const int64_t now_us = esp_timer_get_time();
                /* Frescura: si ha pasado mucho desde la ultima muestra (BLE
                 * caido y reconectado, arranque...), no evaluar un "cruce"
                 * comparando contra un valor de otra epoca -- solo fijar la
                 * base de nuevo. Sin esto, reconectar tras un rato sin enlace
                 * podia disparar un jingle falso (o callarse uno real)
                 * comparando dos lecturas sin relacion temporal entre si.
                 * 60 s de margen: el BLE anuncia cada pocos cientos de ms en
                 * uso normal. Detectado auditando el 08-sep-2026. */
                const bool stale_gap = (s_last_soc_us == 0) ||
                                       (now_us - s_last_soc_us > 60LL * 1000000LL);
                if (s_last_soc >= 0 && !stale_gap) {
                    /* Cruce a la baja del umbral critico */
                    if (s_last_soc >= crit_th && soc_pct < crit_th && !s_crit_active) {
                        s_crit_active = true;
                        /* Encolar (no sonar aqui): esto corre en la task NimBLE
                         * con el lock LVGL cogido; sonar sincrono lo congelaria
                         * ~0.3-1s. La task de audio lo toca fuera del lock. */
                        ui_enqueue_jingle(AUDIO_JINGLE_CRITICAL, true);
                    }
                    /* Recuperacion */
                    if (soc_pct >= crit_th + 2) s_crit_active = false;
                    /* Cruce a la baja del warning (solo si no ya en critico) */
                    if (s_last_soc >= warn_th && soc_pct < warn_th && soc_pct >= crit_th && !s_warn_active) {
                        s_warn_active = true;
                        ui_enqueue_jingle(AUDIO_JINGLE_WARNING, true);
                    }
                    if (soc_pct >= warn_th + 2) s_warn_active = false;
                }
                s_last_soc = soc_pct;
                s_last_soc_us = now_us;
            }
            break;
        }
        case VICTRON_BLE_RECORD_SOLAR_CHARGER:
            /* Con la tension se puede dibujar tambien la potencia que entra a
             * la bateria, y compararla con la que da el panel. */
            battery_history_update_latest(BH_SRC_SOLAR_CHARGER,
                (int32_t)d->record.solar.battery_current_deci * 100,
                (int32_t)d->record.solar.battery_voltage_centi);
            /* Potencia del PANEL (produccion real): va aparte de la corriente
             * que entra a la bateria, porque parte puede ir directa al consumo. */
            battery_history_update_pv((int32_t)d->record.solar.pv_power_w);
            solar_daily_on_pv((int32_t)d->record.solar.pv_power_w);
            break;
        case VICTRON_BLE_RECORD_ORION_XS:
            /* Corriente Y tension de salida (lado bateria). Con la tension se
             * puede saber cuantos kWh ha metido el DC-DC, no solo cuando estuvo
             * cargando. */
            battery_history_update_latest(BH_SRC_ORION_XS,
                (int32_t)d->record.orion.output_current_deci * 100,
                (int32_t)d->record.orion.output_voltage_centi);
            break;
        case VICTRON_BLE_RECORD_AC_CHARGER:
            /* Idem para el cargador de 230 V: con su tension se puede repartir
             * cuanto ha cargado cada fuente. */
            battery_history_update_latest(BH_SRC_AC_CHARGER,
                (int32_t)d->record.ac_charger.battery_current_1_deci * 100,
                (int32_t)d->record.ac_charger.battery_voltage_1_centi);
            break;
        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            /* Orion Tr (0x04): no da corriente, solo estado. Pulso on/off en la
             * serie OrionTR: altura fija mientras carga, 0 cuando no -> se ve
             * cuando y cuanto tiempo carga. El "total Ah" de esta serie sera
             * altura*tiempo, NO amperios-hora reales. */
            uint8_t st = d->record.dcdc.device_state;
            bool charging = (st == VIC_STATE_BULK || st == VIC_STATE_ABSORPTION ||
                             st == VIC_STATE_FLOAT || st == VIC_STATE_STORAGE ||
                             st == VIC_STATE_EQUALIZE || st == VIC_STATE_POWER_SUPPLY);
            battery_history_update_latest(BH_SRC_ORION_XS,
                charging ? ORION_TR_ON_MILLIAMPS : 0, 0);
            break;
        }
        default:
            break;
    }
}
