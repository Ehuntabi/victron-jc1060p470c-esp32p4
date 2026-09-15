/* alarma_estado.h - Estado de las cuatro alarmas, en un solo sitio.
 *
 * POR QUE EXISTE (14-sep-2026): hasta ahora el estado de alarma vivia DENTRO
 * de la vista general (los alarm_*_muted y los estados calculados en
 * overview_render). Eso valia mientras el unico que preguntaba era la propia
 * pantalla, pero ahora hay tres consumidores que no son esa vista y que no
 * pueden depender de que este dibujada:
 *   - el que manda la telemetria UDP (main/net/udp_tx.c), que corre en su
 *     propia tarea y necesita el byte de alarmas activas para la cabina;
 *   - el que atiende la orden de silencio que llega de la cabina
 *     (POST /api/alarma, en la tarea del portal, que no es la de LVGL);
 *   - la propia pantalla, que pinta el parpadeo, el aviso y el icono.
 *
 * Ademas arregla un agujero del diseno anterior: el pitido de las alarmas de
 * AGUA y BATERIA solo se evaluaba dentro de overview_render(), que hoy corre
 * siempre (el tick de la vista se crea una vez y no se para por estar oculta),
 * pero colgaba de que la vista siguiera existiendo. Aqui la evaluacion tiene su
 * propio temporizador y no depende de ninguna vista.
 *
 * SE CALLA SOLO EL SONIDO (decision del usuario, 13-sep-2026): silenciar corta
 * el pitido, y el parpadeo, el aviso y el byte de alarmas activas SIGUEN. El
 * silencio se rearma solo cuando la alarma se recupera: si vuelve a pasar,
 * vuelve a sonar.
 *
 * Hilos: los dos booleanos por alarma son volatile y se escriben con
 * asignaciones de un byte, asi que leerlos desde otra tarea (udp_tx, el portal)
 * no necesita cerrojo. Las llamadas de aqui no tocan LVGL, salvo el propio
 * temporizador de evaluacion (lv_timer, corre en la tarea LVGL), que ademas
 * pinta el aviso flotante sobre lv_layer_top() e interrumpe el salvapantallas
 * -- antes eso vivia en view_overview.c y solo funcionaba con el Overview
 * dibujandose.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cual de las cuatro. Los valores NO son el bitmask del protocolo: aqui son un
 * indice de array, y el bitmask se compone en alarma_estado_bits(). */
typedef enum {
    ALARMA_AGUA = 0,        /* agua limpia en reserva */
    ALARMA_GRISES,          /* aguas grises llenas   */
    ALARMA_BATERIA,         /* SoC por debajo del umbral critico */
    ALARMA_CONGELADOR,      /* frigo por encima de su umbral */
    ALARMA_CUANTAS
} alarma_tipo_t;

/* Arranca la evaluacion periodica (temporizador de LVGL, 500 ms) y la tarea
 * que hace sonar la alarma. Idempotente. Se llama al montar la vista general,
 * pero el temporizador NO depende de ella: sigue corriendo aunque la vista se
 * destruya (una alarma es una alarma tambien con la pantalla en Ajustes o en
 * modo Rotar). */
void alarma_estado_init(void);

/* La condicion se cumple ahora mismo (sonando o silenciada, da igual). */
bool alarma_activa(alarma_tipo_t t);

/* Esta silenciada. Solo puede estar a true si la alarma esta activa: al
 * recuperarse la condicion el silencio se rearma solo. */
bool alarma_silenciada(alarma_tipo_t t);

/* Silencia esa alarma (lo que hace tocar la tarjeta en la P4 y lo que ordena la
 * cabina). Corta el pitido en curso; el parpadeo y el aviso siguen. */
void alarma_silenciar(alarma_tipo_t t);

/* Alterna silencio/sonido. Es lo que hace el icono del altavoz, que es lo unico
 * que puede volver a habilitar el sonido sin esperar a que la alarma se
 * recupere. */
void alarma_alternar_silencio(alarma_tipo_t t);

/* Las cuatro activas en un bitmask MINI_ALARM_* (main/net/mini_proto.h), que es
 * lo que viaja en la telemetria y lo que entiende la orden de la cabina.
 * "Activa" es la condicion, este sonando o no. */
uint8_t alarma_estado_bits(void);

/* Aplica un bitmask MINI_ALARM_* (lo que manda la cabina). Silencia las que
 * vengan en el mask Y esten activas. Deja traza en el log, porque es lo unico
 * que permite reconstruir despues por que sono o dejo de sonar.
 *
 * Devuelve cuantas alarmas ha silenciado de verdad (0 si el mask venia vacio o
 * si ninguna de esas estaba activa). Quien atiende la peticion contesta con ese
 * numero, para que la cabina pueda distinguir "hecho" de "no habia nada que
 * silenciar". */
int alarma_silenciar_mask(uint8_t mask);

/* Texto corto de la alarma, sin acentos (logs y, si hiciera falta, pantalla):
 * "agua", "grises", "bateria", "congelador". */
const char *alarma_nombre(alarma_tipo_t t);

#ifdef __cplusplus
}
#endif
