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

/* Pone a cero ese contador (NVS + RAM). La fecha y el motivo del ultimo
 * arranque NO se tocan: siguen siendo el ultimo reinicio de verdad, que es lo
 * que sirve para diagnosticar. Lo llama el boton "Poner a cero" de
 * Ajustes -> Acerca de, para poder empezar a contar de nuevo. */
void watchdog_clear_reset_count(void);

/* Suspende (true) o reanuda (false) la detección de UI congelada. Para
 * operaciones que se sabe que bloquean LVGL mucho rato sin ser un cuelgue
 * real (p.ej. el borrado de flash al empezar una actualización OTA). */
void watchdog_suspend(bool suspend);


/* Fecha y hora del ultimo arranque QUE NO FUE PARA REPROGRAMAR, en epoch.
 * 0 = no hay dato: o el reloj no estaba puesto, o el ultimo arranque fue una
 * grabacion por cable, una actualizacion OTA o el boton de reiniciar (esos no
 * interesan para diagnosticar, y por eso no se apuntan). */
uint32_t watchdog_arranque_epoch(void);

/* ¿Hay fecha utilizable para el ultimo reinicio apuntado? El motivo puede
 * estar aunque no la haya: con la pila del RTC muerta se apunta sin fecha. */
bool watchdog_arranque_con_fecha(void);

/* Motivo del ultimo arranque que SI se apunto (el de la fecha de arriba), o
 * "sin reinicios apuntados" mientras no haya ninguno: los reinicios de
 * grabacion por cable, las OTA y el boton Reiniciar no se apuntan. */
const char *watchdog_arranque_reason(void);

/* Apunta la fecha del arranque actual. Llamar UNA vez, cuando ya hay reloj
 * (en main.c, despues de rtc_init y de poner la hora). */
void watchdog_anota_arranque(void);

/* Marca que el reinicio que viene lo pide el propio aparato a proposito (OTA o
 * boton Reiniciar). El arranque siguiente lo lee y no lo cuenta como averia. */
void watchdog_marca_reinicio_pedido(void);

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
    WD_TASK_NE185_VLOG,   /* ne185_vlog.c: vlog_flush_task */
    WD_TASK_LOG_CLEANUP,  /* log_cleanup.c: cleanup_task */
    WD_TASK_LVGL,          /* ui.c: lv_timer periodico -- ver wd_monitor_task */
    WD_TASK_COUNT
} wd_task_t;

/* Marca la tarea como viva (latido). Seguro de llamar desde cualquier tarea. */
void watchdog_heartbeat(wd_task_t task);
