#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Inicializa el módulo watchdog:
 *  - Lee la causa del último reset y, si fue por TASK_WDT/INT_WDT/WDT/
 *    CPU_LOCKUP/panic (o un reset forzado por este mismo monitor), incrementa
 *    el contador persistido en NVS (namespace "wd"). Ver is_wdt_reset en
 *    watchdog.c para la lista exacta.
 *  - Crea una task monitor que cada N s comprueba salud de LVGL: si el lock
 *    falla N veces seguidas (UI congelada), fuerza un reset controlado para
 *    que el chip vuelva a un estado limpio -- SIN flush a SD a propósito (ver
 *    el comentario en watchdog.c: si el cuelgue lo causa el propio
 *    subsistema de SD/FAT, intentar volcar ahí podría colgar también el
 *    reset).
 */
esp_err_t watchdog_init(void);

/* Devuelve el contador de resets provocados por watchdog (TWDT/INT_WDT/panic)
 * desde el primer arranque tras último wipe. */
uint32_t watchdog_get_reset_count(void);

/* Suspende (true) o reanuda (false) la detección de UI congelada. Para
 * operaciones que se sabe que bloquean LVGL mucho rato sin ser un cuelgue
 * real (p.ej. el borrado de flash al empezar una actualización OTA). */
void watchdog_suspend(bool suspend);

/* Devuelve la causa del último reset, en formato legible para mostrar en UI
 * ("Power-on", "Watchdog (TWDT)", "Panic", etc.). El puntero apunta a una
 * cadena estática y no debe liberarse. */
const char *watchdog_last_reset_reason(void);

/* Tareas de app vigiladas por heartbeat. Cada una debe llamar a
 * watchdog_heartbeat() en cada iteracion de su bucle principal. Si una deja
 * de latir mas de su umbral (ver WD_TASK_TIMEOUT_US_TABLE en watchdog.c --
 * NO es el mismo para todas: las de volcado a SD laten cada 30-600s, mucho
 * mas lento que NE185/FRIGO), el monitor fuerza un reset controlado. Una
 * tarea que nunca late (p.ej. no arranco) simplemente no se vigila.
 *
 * DL_FLUSH/BH_FLUSH/VIAJE_TICK anadidas el 09-sep-2026: antes un atasco de
 * SD/FAT en cualquiera de estas tres (mutex retenido, tarjeta muerta) no lo
 * detectaba nadie -- la placa seguia "viva" (LVGL respondia) con el volcado
 * a SD parado para siempre. */
typedef enum {
    WD_TASK_NE185 = 0,
    WD_TASK_FRIGO,
    WD_TASK_DL_FLUSH,     /* datalogger.c: flush_task */
    WD_TASK_BH_FLUSH,     /* battery_history.c: bh_flush_task */
    WD_TASK_VIAJE_TICK,   /* config_server_viaje.c: viaje_tick_task */
    WD_TASK_COUNT
} wd_task_t;

/* Marca la tarea como viva (latido). Seguro de llamar desde cualquier tarea. */
void watchdog_heartbeat(wd_task_t task);
