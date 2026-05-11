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
} frigo_log_entry_t;

int  log_browser_load_frigo(const char *path,
                            frigo_log_entry_t *out, int max);

/* Bateria: solo se carga BM (source == "BM"). Devuelve nº de entradas. */
typedef struct {
    int     hh;
    int     mm;
    int32_t milli_amps;
} battery_log_entry_t;

int  log_browser_load_battery(const char *path,
                              battery_log_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
