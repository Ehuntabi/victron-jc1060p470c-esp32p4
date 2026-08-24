#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t reset_epoch;      /* segundos desde epoch; 0 si nunca */
    double wh_charged;        /* energia entrante acumulada */
    double wh_discharged;     /* energia saliente acumulada */
    double ah_charged;        /* carga entrante (A*h) */
    double ah_discharged;     /* carga saliente (A*h) */
    double wh_solar;          /* energia aportada por la placa solar (MPPT) */
    double ah_solar;          /* carga aportada por la placa solar (A*h) */
    int64_t solar_seconds;    /* tiempo con la placa cargando (corriente > 0) */
    int64_t seconds_running;  /* tiempo "activo" (con sample en intervalo) */
    double  km;               /* distancia recorrida segun el GPS */
    bool    active;           /* hay un viaje en curso (ver trip_computer_is_active) */
} trip_computer_t;

/* Carga el estado persistido en NVS (o ceros si no hay). Llamar al boot. */
void trip_computer_init(void);

/* Hook: integra energia/carga entre samples reales del BMV. */
void trip_computer_on_battery(int32_t i_milli, uint16_t v_centi);

/* Hook: integra la energia que aporta la placa solar (MPPT), medida en su
 * salida hacia la bateria. Se acumula aparte de la carga neta del shunt
 * (no es un subconjunto exacto: parte puede ir directa al consumo). */
void trip_computer_on_solar(int32_t i_milli, uint16_t v_centi);

/* Hook del GPS: suma la distancia entre esta posicion y la anterior.
 *
 * Se le llama con lo que haya, tenga fix o no, y el filtrado se hace aqui
 * dentro (asi el criterio vive en un solo sitio). Descarta:
 *  - sin fix o con pocos satelites: la posicion baila decenas de metros;
 *  - saltos por debajo del ruido: un receptor PARADO deriva, y sin este filtro
 *    una noche de camping sumaria kilometros que nadie ha recorrido;
 *  - saltos imposibles y reenganches tras perder la senal: se re-ancla en el
 *    punto nuevo SIN contar la linea recta, que no es por donde se fue.
 * Por eso el total peca por defecto: prefiere quedarse corto a inventar. */
void trip_computer_on_gps(bool fix, uint8_t satelites, double lat, double lon);

/* Guarda los contadores en NVS ahora mismo (normalmente se hace cada 5 min).
 * Antes de sacar la tarjeta o apagar. */
void trip_computer_flush(void);

/* Pone todos los contadores a cero y deja el viaje abierto. Guarda en NVS.
 *
 * Lo llama el INICIO DE VIAJE del cuaderno de la cabina (/api/viaje), no el
 * usuario: el apartado ENERGIA del resumen.txt sale de estos contadores, y si
 * solo se pusieran a cero a mano el resumen acabaria contando energia de
 * viajes anteriores como si fuera de este. En Ajustes queda el mismo boton
 * como puesta a cero suelta, para cuando se quiera medir algo aparte. */
void trip_computer_reset(void);

/* Marca el viaje como TERMINADO y guarda los contadores. Contraparte de
 * trip_computer_reset: lo llama el fin de viaje, despues de escribir el
 * resumen (que es quien lee estos numeros). */
void trip_computer_end(void);

/* true si hay un viaje abierto en los contadores. */
bool trip_computer_is_active(void);

/* Copia el snapshot actual a out. Thread-safe (mutex interno). */
void trip_computer_get(trip_computer_t *out);

#ifdef __cplusplus
}
#endif
