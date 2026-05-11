#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa el acumulador del día. Lee fecha actual del RTC y, si coincide
 * con el día persistido en NVS, restaura los valores; si no, empieza a 0. */
void energy_today_init(void);

/* Llama cuando llegue un record BMV: integra la energía de carga/descarga
 *   - i_milli > 0: corriente entrando → suma a PV/charge_kwh
 *   - i_milli < 0: corriente saliendo → suma a loads_kwh
 * Internamente usa time(NULL) para calcular delta horario.
 * Detecta cambio de día y resetea acumuladores si pasa medianoche. */
void energy_today_on_battery(int32_t i_milli, uint16_t v_centi);

/* Llama cuando llegue un record SmartSolar con yield_today_centikwh (0.01 kWh).
 * Este valor lo proporciona el propio cargador (más fiable que integrar). */
void energy_today_on_solar_yield(uint16_t yield_centikwh);

/* Devuelven kWh acumulados hoy (a 0.001 = mWh de precisión interna). */
float energy_today_pv_kwh(void);       /* charge_kwh (BMV) o yield SmartSolar */
float energy_today_loads_kwh(void);

/* Devuelve true si hubo cambio de día desde última llamada (uso interno
 * principalmente). */
bool  energy_today_is_fresh(void);

#ifdef __cplusplus
}
#endif
