v4.3 — los históricos se leen por bloques (2,3x más rápido) y v4.02/v4.01/v4.00 quedan documentadas

## Qué cambia

- **`main/log_browser.c`**: los dos lectores de históricos (`log_browser_load_frigo`
  y `log_browser_load_battery`, que son los que usan las pantallas de frigo y
  batería) ya no leen **línea a línea con `fgets`**: leen bloques de 64 KB con
  `read()` y parten las líneas en memoria buscando los `\n`. El parseo de cada
  línea es exactamente el mismo de antes.
- El motivo, medido con el CSV real (1,26 MB, 25.921 líneas):
  `fgets` copia el texto carácter a carácter por la capa de stdio y eso domina;
  leer a bloques y trocear en RAM era **9,8x** más rápido solo en el recorrido
  (0,25 → 2,43 MB/s, con el mismo número de líneas contadas).

## Medido con el lector de verdad (el de la pantalla de batería)

| | tiempo | notas |
|---|---|---|
| Recorrido `fgets` solo (sin parsear) | 2,97 s | lo que costaba el camino viejo de leer |
| **Lector nuevo completo** (lee + parsea) | **1,51 s** | 25.920 entradas, BM/solar/orion 8.640 cada una |
| Camino viejo completo (estimado) | ~3,5 s | recorrido + el mismo parseo |

O sea **~2,3x** en abrir un día de batería. Y ahora el cuello es el **parseo**
(~1,3 s de los 1,5 s: `csv_split` + `strtol` + `strcmp` por línea), no la tarjeta:
si algún día molesta, el siguiente paso es un parser más directo.

## Trampa que me comí (y queda escrita en el código)

La primera versión del lector por bloques tardaba **8,3 s** (peor que `fgets`):
movía el resto del bloque con `memmove` **en cada línea** (64 KB × 25.920 =
1,6 GB copiados). Se arregló avanzando un puntero de consumo dentro del bloque y
compactando solo al reponer. Queda comentado en `lb_linea()` para que no se
repita.

## Nota sobre los números de versión

- Publicadas: **v4.00**, **v4.01**, **v4.02** y esta **v4.3**.
- Los builds del árbol de scratch (los de las pruebas de banco) calculan su
  versión como "último tag + 0,1", así que dicen **v4.1** (desde v4.00) o
  **v4.3** (desde v4.02). Si en la pantalla aparece un `v4.1`, es un firmware de
  pruebas, no un release.
