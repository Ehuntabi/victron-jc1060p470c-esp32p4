#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Historico de produccion solar DIA A DIA, pensado para comparar temporadas:
 * el usuario va a doblar la placa (125 W -> 250/300 W) en septiembre y quiere
 * ver la mejora. Por eso se guarda un ano entero, no unos dias.
 *
 * Vive en la SD (/sdcard/solar/produccion.csv, una linea por dia) y se carga en
 * RAM al arrancar. El dia en curso se persiste tambien en NVS cada pocos
 * minutos para no perderlo en un reinicio. */

#define SOLAR_DAILY_MAX_DAYS  400

typedef struct {
    int32_t day_id;      /* dias desde epoch (time/86400 en hora local) */
    float   kwh;         /* produccion del panel ese dia */
    float   horas;       /* tiempo con el panel produciendo (> umbral) */
    int32_t pico_w;      /* maximo instantaneo del dia */
    float   kwh_consumo; /* energia consumida ese dia, medida en el shunt (BMV) */
} solar_day_t;

/* Carga el historico de la SD y el dia en curso de NVS. Llamar tras montar SD. */
void solar_daily_init(void);

/* Hook: potencia del panel (W) que informa el cargador solar. Integra energia y
 * tiempo usando el hueco real entre llamadas. */
void solar_daily_on_pv(int32_t watts);

/* Hook: lectura del shunt (BMV). Integra SOLO la descarga (corriente negativa)
 * para saber cuanta energia se ha consumido en el dia. Asi se puede comparar
 * produccion contra consumo. */
void solar_daily_on_battery(int32_t milli_amps, int32_t centi_volts);

/* Dia en curso (no cerrado todavia). */
void solar_daily_get_today(solar_day_t *out);

/* Copia los ultimos n dias CERRADOS, del mas antiguo al mas reciente.
 * Devuelve cuantos ha copiado. */
int solar_daily_get_days(solar_day_t *out, int max);

/* Media de kWh/dia de los ultimos n dias cerrados (0 si no hay datos). */
float solar_daily_avg(int dias);

/* Media de consumo kWh/dia de los ultimos n dias cerrados. */
float solar_daily_avg_consumo(int dias);

/* Fuerza el guardado (al apagar, o antes de operaciones largas de SD). */
void solar_daily_flush(void);

#ifdef __cplusplus
}
#endif
