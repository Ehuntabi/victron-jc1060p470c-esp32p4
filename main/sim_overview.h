/* sim_overview.h — Activacion del modo simulacion */
#pragma once

/* Cambia a 0 para desactivar la simulacion (modo produccion). */
#define SIM_OVERVIEW_ENABLE  0

#ifdef __cplusplus
extern "C" {
#endif

void sim_overview_start(void);

#ifdef __cplusplus
}
#endif
