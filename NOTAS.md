v4.4 — el parseo de los históricos a mano: 2,78x más rápido y con los mismos números

## Qué cambia

`main/log_browser.c` (los lectores de las pantallas de frigo y batería):

- **`parse_hhmm` ya no usa `sscanf`**: el timestamp lo escribe el propio proyecto
  con formato fijo (`"%04d-%02d-%02d %02d:%02d:%02d"`), así que la hora y el
  minuto se leen por posición (11-12 y 14-15, comprobando los dígitos y el `:`).
  `sscanf` es la función más cara de la librería y se llamaba **una vez por
  línea**.
- **Enteros a mano** (`parse_int`) en vez de `strtol` (genérico: signo, base,
  espacios, desbordamiento).
- **La fuente, por su primera letra** (`B`atteryMonitor, `S`olarCharger,
  `O`rionTR, `A`CCharger) en vez de hasta 4 `strcmp` por línea, con el `strcmp`
  completo detrás como comprobación (un "BananaMonitor" de un CSV futuro no se
  cuela como BatteryMonitor).

## Verificado: mismos números y más rápido

Mismo CSV de batería (1,26 MB, 25.920 muestras), en el mismo arranque:

| | tiempo | velocidad |
|---|---|---|
| camino viejo (`fgets` + `sscanf` + `strtol` + `strcmp`) | 2,86 s | 0,420 MB/s |
| **lector nuevo** (bloques + parseo a mano) | **1,03 s** | **1,170 MB/s** (**2,78x**) |

Y la comparación **entrada por entrada** (hora, minuto, amperios y voltios de las
25.920 muestras): **idéntica**.

Con esto, abrir un día de batería pasa de ~3 s (v4.02) a ~1 s, y ya no queda
cuello claro: leer el fichero y trocear las líneas es ~0,5 s y el parseo ~0,5 s.
