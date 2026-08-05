#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BROWSER_MAX_DATES   60
#define LOG_BROWSER_DATE_LEN    11   /* "YYYY-MM-DD" + NUL */

/* Rellena `dates_out` (orden ascendente) con las fechas YYYY-MM-DD encontradas
 * en `dir` como archivos YYYY-MM-DD.csv. Devuelve el numero de fechas. */
int  log_browser_list_dates(const char *dir,
                            char dates_out[][LOG_BROWSER_DATE_LEN],
                            int max);

/* Frigo: entrada CSV "timestamp,Ta,Tc,Te,fan_pct". Devuelve nº de entradas. */
typedef struct {
    int   hh;        /* hora local extraida del timestamp */
    int   mm;
    float t_aletas;  /* NAN si vacio */
    float t_congel;
    float t_exter;
    int   fan_pct;
    int   excedente_solar;   /* 0/1; 0 si el CSV no tiene la columna (formato viejo) */
} frigo_log_entry_t;

int  log_browser_load_frigo(const char *path,
                            frigo_log_entry_t *out, int max);

/* Bateria: entrada CSV "timestamp,source,mA,mA_max,mA_min,cV,pv_W". */
typedef struct {
    int     hh;
    int     mm;
    int32_t milli_amps;
    int32_t centi_volts;   /* 0 si el CSV no tiene columna de tension (formato viejo) */
} battery_log_entry_t;

/* Carga las BH_SRC_COUNT fuentes del CSV, cada una en su buffer: out[s] recibe
 * hasta `max` entradas de la fuente s y n_out[s] cuantas se leyeron. Un buffer
 * a NULL se salta (esa fuente no se parsea). Devuelve el total de entradas.
 *
 * OJO: tarda segundos (un dia son decenas de miles de lineas) y toma el cerrojo
 * de la SD. NO llamarla desde un callback de LVGL: con el cerrojo de LVGL
 * retenido >9 s el watchdog SW da la UI por congelada y reinicia la placa.
 * En la UI la llama bh_loader_task (main/ui.c). */
int  log_browser_load_battery(const char *path,
                              battery_log_entry_t *const *out, int max,
                              int *n_out);

#ifdef __cplusplus
}
#endif
