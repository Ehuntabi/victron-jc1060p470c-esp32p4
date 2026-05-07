#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Carga preferencias de NVS al arrancar (llamar tras nvs_flash_init) */
void alerts_init(void);

/* Frigo: minutos subiendo + temp umbral */
int   alerts_get_freezer_minutes(void);   /* default 30 */
void  alerts_set_freezer_minutes(int min);
float alerts_get_freezer_temp_c(void);    /* default -2 */
void  alerts_set_freezer_temp_c(float t);

/* Bateria: umbral SoC critico + warning (en porcentaje 0..100) */
int  alerts_get_soc_critical(void);  /* default 30 */
void alerts_set_soc_critical(int pct);
int  alerts_get_soc_warning(void);   /* default 60 */
void alerts_set_soc_warning(int pct);

#ifdef __cplusplus
}
#endif
