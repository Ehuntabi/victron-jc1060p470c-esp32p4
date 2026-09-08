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
