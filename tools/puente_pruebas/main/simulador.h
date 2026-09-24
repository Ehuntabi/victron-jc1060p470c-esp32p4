/* simulador.h — emisor de datos de prueba (BLE y WiFi) para ejercitar la P4.
 * Herramienta de banco: ver LEEME.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* BLE: emite con esos dispositivos (hasta 4), rotando por TODOS los tipos de
 * registro que entiende la P4. La clave va en hex (32 caracteres). */
bool sim_ble_iniciar(int n, const char *macs[], const char *claves[]);
void sim_ble_parar(void);

/* Ritmo entre tramas, en milisegundos (200 por defecto; 20 = a tope). */
void sim_ble_ritmo(int ms);

/* Modo CAOS: va rotando solo por normal -> valores fijos -> extremos -> tramas
 * malformadas, para machacar todos los caminos del parser sin tocar el PC. */
void sim_ble_caos(bool activar);
const char *sim_ble_modo(void);
void sim_ble_informe(void);
bool sim_ble_activo(void);

/* Calla el resumen periodico (RESUMEN ...) mientras se vuelca un fichero por la
 * consola: esa linea se colaba EN MEDIO de una linea base64 y rompia la
 * captura. Solo afecta al resumen; el simulador sigue emitiendo igual. */
void sim_silencio(bool on);

/* Modo extremo: manda centinelas de "sin dato" (0x7FFF/0xFFFF), maximos de 32
 * bits, SOC fuera de rango y temperaturas imposibles. Es para BUSCAR PROBLEMAS:
 * desbordamientos en las cuentas de energia, valores raros en pantalla, etc. */
/* Modo de valores FIJOS: emite siempre el registro de monitor de bateria con
 * estos numeros, para poder comparar exactamente lo enviado con lo que la P4
 * publique despues en su telemetria. */
void sim_ble_fijo(bool activar, int soc_deci, int v_centi, int i_milli);

void sim_ble_extremo(bool activar);
bool sim_ble_extremo_activo(void);
