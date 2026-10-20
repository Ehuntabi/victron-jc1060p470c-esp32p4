#pragma once

/* stack_watch.c — log de uxTaskGetStackHighWaterMark() 1x/min de las 5
 * tareas de vuelco a SD (ver el bootloop del 08-sep-2026, bh_flush_task).
 * uxTaskGetStackHighWaterMark ya es el MINIMO historico de pila libre desde
 * que arranco la tarea, asi que con 24-48h corriendo el ultimo valor logueado
 * es el peor caso real visto -- eso es lo que hay que mirar para decidir si
 * los tamanos actuales (ver los xTaskCreate de cada modulo) sobran o siguen
 * ajustados, en vez de adivinar.
 *
 * Diagnostico temporal: quitar la llamada en main.c cuando ya no haga falta
 * seguir midiendo (no molesta si se deja, pero no aporta nada pasado ese
 * punto). */

void stack_watch_start(void);

/* Cerrojo de la medicion (tanda 4 de la auditoria del 18-oct-2026). Quien vaya a
 * BORRAR una de las tareas vigiladas lo coge, pone su handle a NULL y la borra:
 * asi uxTaskGetStackHighWaterMark() no puede caer sobre un TCB ya liberado (el
 * caso de viaje_tick_task cuando no se pudo crear su timer). */
void stack_watch_bloquea(void);
void stack_watch_suelta(void);
