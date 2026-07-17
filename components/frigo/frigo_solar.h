/* frigo_solar.h - Maquina de estados PURA del modo "aprovechar excedente solar".
 * Sin dependencias de hardware ni FreeRTOS: testeable en host (gcc). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constantes fijas (no ajustables por UI). */
#define FRIGO_SOLAR_PV_MIN_W        80u              /* W minimos de panel = "hay sol real" */
#define FRIGO_SOLAR_MIN_ON_MS       (30u*60u*1000u)  /* bloque minimo encendido (30 min) */
#define FRIGO_SOLAR_ACT_DEBOUNCE_MS (60u*1000u)      /* condiciones sostenidas antes de activar */

/* Entradas de una evaluacion (valores ya escalados como en dashboard_state/ne185). */
typedef struct {
    bool     enabled;      /* toggle maestro del modo */
    uint16_t soc_deci;     /* SoC en 0.1 %  (960 = 96.0 %) */
    uint16_t pv_w;         /* potencia del panel en W */
    bool     shore;        /* hay 230 V (NE185) */
    bool     fresh;        /* telemetria Victron Y NE185 recientes */
    uint8_t  soc_on_pct;   /* umbral de activacion (%) */
    uint8_t  soc_off_pct;  /* suelo de corte (%) */
    uint32_t now_ms;       /* reloj monotonico en ms */
} frigo_solar_in_t;

/* Estado persistente entre evaluaciones (RAM, no NVS). Inicializar a {0}. */
typedef struct {
    bool     active;           /* salida actual del rele */
    uint32_t active_since_ms;  /* instante de entrada al estado actual */
    bool     arming;           /* condiciones de activacion cumpliendose */
    uint32_t arming_since_ms;  /* desde cuando */
} frigo_solar_sm_t;

/* Evalua la maquina de estados. Actualiza *sm y devuelve el nivel del rele
 * (true = ON = frigo a 12V por excedente). Funcion pura. */
bool frigo_solar_eval(const frigo_solar_in_t *in, frigo_solar_sm_t *sm);

#ifdef __cplusplus
}
#endif
