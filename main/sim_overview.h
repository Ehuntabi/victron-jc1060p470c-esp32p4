/* sim_overview.h — Activacion del modo simulacion */
#pragma once

/* Cambia a 0 para desactivar la simulacion (modo produccion). */
#define SIM_OVERVIEW_ENABLE  0

#ifdef __cplusplus
extern "C" {
#endif

void sim_overview_start(void);

/* Devuelve a su nombre cualquier "<csv>.real" que el simulador dejara apartado
 * en /sdcard/frigo y /sdcard/bateria, borrando el inventado que lo tapaba.
 * Con el simulador ENCENDIDO no hace nada. Llamar al arrancar, despues de
 * montar la SD y ANTES de que el registrador escriba el csv de hoy. */
void sim_overview_restaurar_reales(void);

#ifdef __cplusplus
}
#endif
