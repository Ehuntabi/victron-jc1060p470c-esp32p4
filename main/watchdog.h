#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Inicializa el módulo watchdog:
 *  - Lee la causa del último reset y, si fue por TWDT/INT_WDT/panic, incrementa
 *    el contador persistido en NVS (namespace "wd").
 *  - Crea una task monitor que cada N s comprueba salud de LVGL: si el lock
 *    falla N veces seguidas (UI congelada), hace flush a SD y fuerza reset
 *    controlado para que el chip vuelva a un estado limpio.
 */
esp_err_t watchdog_init(void);

/* Devuelve el contador de resets provocados por watchdog (TWDT/INT_WDT/panic)
 * desde el primer arranque tras último wipe. */
uint32_t watchdog_get_reset_count(void);

/* Devuelve la causa del último reset, en formato legible para mostrar en UI
 * ("Power-on", "Watchdog (TWDT)", "Panic", etc.). El puntero apunta a una
 * cadena estática y no debe liberarse. */
const char *watchdog_last_reset_reason(void);
