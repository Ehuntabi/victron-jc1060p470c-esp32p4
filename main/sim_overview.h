/* sim_overview.h — Activacion del modo simulacion */
#pragma once

/* Cambia a 1 para activar la simulacion (solo para pruebas de banco).
 * Se encendio el 7-oct-2026 para validar el camino de datos con la cabina nueva
 * (los datos ficticios entran por ui_on_panel_data -> dashboard_state, o sea que
 * salen por UDP igual que los de verdad) y se apago al terminar. */
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
