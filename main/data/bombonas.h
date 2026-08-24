#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registro de CAMBIOS de bombona, para saber cuanto dura cada una.
 *
 * Ojo con la distincion, que es el motivo de que esto exista: COMPRAR una
 * bombona es un gasto y ya lo apunta el cuaderno de la cabina ("bombona" en los
 * registros del viaje). CAMBIARLA es otra cosa y ocurre en otro momento --
 * puedes llevar una de repuesto meses en el hueco. Lo que mide la duracion es
 * el cambio, no la compra.
 *
 * Se guardan solo las fechas de cambio; la duracion de cada bombona es la resta
 * entre dos cambios consecutivos. Por eso la bombona EN CURSO no tiene duracion
 * todavia: se sabra cuando se cambie la siguiente. */

#define BOMBONAS_MAX 12   /* ~12 cambios de historico; a 2 meses cada una, 2 anos */

typedef struct {
    uint32_t cambios[BOMBONAS_MAX];  /* epoch de cada cambio, del mas VIEJO al mas nuevo */
    uint8_t  n;                      /* cuantos hay guardados */
} bombonas_t;

/* Carga lo guardado en NVS. Llamar una vez al arrancar. */
void bombonas_init(void);

/* Apunta un cambio de bombona AHORA. Devuelve false si el reloj todavia no
 * esta en hora (sin fecha buena, apuntarlo seria inventar un dato) o si no se
 * ha podido guardar. */
bool bombonas_cambio(void);

/* Copia el registro. Thread-safe. */
void bombonas_get(bombonas_t *out);

/* Deshace el ultimo cambio apuntado. Para el dedazo: si no se puede corregir,
 * un toque accidental estropea la estadistica para siempre. */
bool bombonas_deshacer(void);

/* Dias que lleva puesta la bombona actual, o -1 si no hay ningun cambio
 * apuntado todavia (o el reloj no esta en hora). */
int bombonas_dias_actual(void);

/* true si la bombona actual lleva MAS dias que la media de las anteriores, o
 * sea que por estadistica puede estar al caer. false si no hay media todavia
 * (hacen falta dos cambios) o si aun no ha llegado. */
bool bombonas_pasada(void);

/* Duracion media en dias de las bombonas YA terminadas, o -1 si aun no hay
 * ninguna terminada (hacen falta dos cambios para saber cuanto duro una). */
float bombonas_media_dias(void);

#ifdef __cplusplus
}
#endif
