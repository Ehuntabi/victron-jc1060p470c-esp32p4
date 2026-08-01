#pragma once
#include "esp_err.h"

/* Log de comparacion de voltaje: bytes 12/13 CRUDOS del NE185 junto al voltaje
 * que da el SmartShunt por BLE, para deducir la formula real del NE185.
 *
 * Motivo: `spec_ne185.md` (repo class142/ne-rs485, PR #4) documenta
 * V = (raw - 30)/10, heredada del NE334 y NUNCA medida contra un polimetro,
 * porque en esta autocaravana el voltaje se lee del shunt, no del bus RS-485.
 * Con unas semanas de pares (raw, voltaje_real) sale la formula por regresion.
 *
 * NO pinta nada en pantalla: solo escribe CSV en la SD.
 * Ficheros: /sdcard/ne185v/AAAA-MM-DD.csv  (o boot.csv si aun no hay hora).
 */

esp_err_t ne185_vlog_init(void);

/* Vuelca a la SD lo que quede en RAM. Llamar antes de un reinicio previsto. */
void ne185_vlog_flush(void);
