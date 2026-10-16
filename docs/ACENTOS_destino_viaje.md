# Destino del viaje: UN solo criterio antes de levantar el filtro ASCII

(16-oct-2026, del BLOQUE 4 de `PENDIENTE_16oct.md`. **Copia idéntica en los tres
repos**: `victron/docs/`, `cabina5/docs/` y `35cabina/docs/`; se sincroniza a mano
como `mini_proto.h`.)

## El problema

Hoy hay DOS trozos de código que convierten el destino que teclea el usuario en un
nombre de carpeta, uno en cada placa:

| dónde | función | qué hace |
|---|---|---|
| cabina (5" y 3,5") | `main/salida.c:218-247` (`sanear()`) | translitera á→a … ñ→n, y **todo separador (`-`, `_`, espacio) acaba en `_`** |
| P4 | `main/portal/config_server_viaje.c` (`destino_seguro()`) | misma tabla de acentos, pero **el guion se queda en `-`** y el espacio/`_` van a `_` |

Además la cabina **restringe la entrada a ASCII** (`main/ui/view_registro.c:6116-6130`),
así que hoy los acentos ni siquiera llegan a la P4 desde la pantalla: el problema
está tapado, no resuelto.

Divergencias reales medidas comparando las dos funciones HOY:

1. **El guion**: `A Coruña-Bilbao` → cabina `A_Coruna_Bilbao`, P4 `A_Coruna-Bilbao`.
   Dos nombres distintos para el mismo viaje.
2. **La longitud**: la 3,5" limita el destino a 20 caracteres; la P4 tiene
   `DESTINO_MAX 24` como cota defensiva. Si el filtro de la cabina se levanta, los
   dos topes tienen que ser el mismo número.
3. **El vacío**: las dos acaban en `"viaje"` (bien, pero por caminos distintos).

Mientras sigan así, da igual cuál se elija: lo que NO se puede es levantar el
filtro de acentos de la cabina y dejar que cada placa nombre la carpeta a su
manera, porque el `CSV`/historial de la cabina y la carpeta de la SD de la P4
dejarían de corresponderse y el "estado del viaje" se decidiría sobre un nombre
que no es el que existe.

## El criterio (UNO, para las tres placas)

Se adopta **la regla de la P4** (`destino_seguro()`), que es la que nombra la
carpeta de verdad en la SD, y la cabina cambia su `sanear()` para que dé
exactamente el mismo resultado:

1. Se recorre el UTF-8 byte a byte.
2. Si el carácter es `0xC3 0xXX` y `0xXX` está en la tabla, se emite la letra sin
   acento:
   `á é í ó ú Á É Í Ó Ú ñ Ñ ç Ç ü Ü` → `a e i o u A E I O U n N c C u U`.
3. Cualquier otro byte no ASCII (`0x80`-`0xFF`, emoji, comillas tipográficas…) se
   **tira** (no se deja hueco ni se convierte en `_`).
4. Se conservan `a-z A-Z 0-9` y el **guion `-`** tal cual.
5. Espacio, `_` y cualquier otro separador → `_`.
6. No se acumulan separadores (`--`, `__`, `-_` → uno solo) y el nombre no puede
   empezar ni terminar en `_`.
7. Tope: **20 caracteres** en las tres placas (el que ya usa la 3,5"); si al
   recortar queda vacío o el nombre entero se queda vacío → `viaje`.
8. La función tiene que ser **idempotente**: `f(f(x)) == f(x)` (se aplica al recibir
   y otra vez al componer la ruta).

## Cómo se comprueba (vectores que TIENEN que coincidir byte a byte)

| entrada | salida |
|---|---|
| `Cádiz` | `Cadiz` |
| `Cataluña` | `Cataluna` |
| `A Coruña-Bilbao` | `A_Coruna-Bilbao` |
| `  doble  espacio  ` | `doble_espacio` |
| `a--b` | `a-b` |
| `ñçü` | `ncu` |
| `   ` | `viaje` |
| `./../etc/passwd` | `etc_passwd` |
| `2026-10-11_ruta` | `2026-10-11_ruta` |
| `Ünïcödé` | `Unicode` |

`sanear()` y `destino_seguro()` se comprueban con la MISMA tabla: el test que ya
extrae y compila la función real de la P4 es `victron/test/test_viaje_dueno.sh`
(compila `destino_seguro` de verdad, no una copia), así que esos casos van ahí; en
las cabinas, el equivalente es `test/viaje_*.sh`, que ya extraen funciones reales
de `main/`.

## Lo que hay que tocar (fuera del territorio de los scripts)

- `cabina5/main/salida.c` + `35cabina/main/salida.c`: cambiar `sanear()` (regla 4 y
  tope 20) y **no** duplicar la tabla.
- `cabina5/main/ui/view_registro.c` + `35cabina/main/ui/view_registro.c`: levantar
  el filtro ASCII del destino SOLO cuando las dos funciones coincidan (hoy bloquea
  los acentos; es lo que evita que el problema se vea).
- `victron/main/portal/config_server_viaje.c`: `destino_seguro()` ya sigue esta
  regla; lo que hay que hacer es **no cambiarla** y, si se toca, actualizar la copia
  de las cabinas en el mismo commit (los ficheros de `main/` no están en el espejo
  del CI, así que nada avisará).
