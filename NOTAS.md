v4.6 — el reintento de montaje de la SD corta la corriente 1,5 s (no 300 ms)

## Qué cambia

En `components/datalogger/datalogger.c`, el corte de corriente a la tarjeta pasa a
tener dos tiempos:

- **Arranque (primer intento): 300 ms**, como siempre. El camino normal no cambia.
- **Reintento: 1500 ms** (`SD_CORTE_REINTENTO_MS`).

## Por qué

Medido el 2-oct-2026: un arranque con la tarjeta en mal estado falló **los 3
intentos** al leer el SCR (`sdmmc_init_sd_scr: send_scr (1) returned 0x107`), y
cada reintento volvía a cortar solo 300 ms. Si con 300 ms la tarjeta no suelta,
repetir 300 ms no aporta nada: hay que dar tiempo a que el rail de 3V3 (100 µF del
conector más lo que lleva la propia tarjeta) baje de verdad y a que el controlador
interno haga su reset. 1500 ms son ~5 constantes de tiempo de ese rail, y solo se
pagan cuando el primer intento ya ha fallado.

## Verificado

- **Camino normal intacto**: arranque con la tarjeta bien → monta al primer
  intento, mismo tiempo que antes (corte de 300 ms).
- **Reintento, forzando el fallo del primer intento** en el árbol de pruebas:
  el intento 1 falla en t=3547 ms, el corte largo dura **1501 ms** (3547 → 5048) y
  el intento 2 monta a 4 bits y 40 MHz en t=5516 ms. El mecanismo hace lo que dice.
- Regla nueva en `test/auditar.sh` (sección 6): el reintento tiene que cortar más
  que el arranque, para que nadie lo "simplifique" de vuelta.

## Lo que NO se puede afirmar

El fallo de campo (tarjeta que no suelta con 300 ms) **no se puede reproducir a
voluntad**, así que la mejora está razonada y el mecanismo verificado, pero no
medida contra ese fallo concreto. Lo que sí está medido es que el camino normal no
cambia y que el reintento ejecuta el corte largo.
