# Destino del viaje: UN solo criterio antes de levantar el filtro ASCII

(16-oct-2026, del BLOQUE 4 de `PENDIENTE_16oct.md`. **Revisado el 17-oct-2026**
contra el código, después del arbitraje: ese día `destino_seguro()` cambió al
criterio de la cabina (los tres separadores → `_`), y la tabla de vectores de
abajo es la que sale HOY, no la que se quería. **Copia idéntica en los tres
repos**: `victron/docs/`, `cabina5/docs/` y `35cabina/docs/`; se sincroniza a
mano como `mini_proto.h`.)

## El problema

Dos trozos de código convierten el destino que teclea el usuario en un nombre de
carpeta, uno por placa:

| dónde | función | qué hace |
|---|---|---|
| cabina (5" y 3,5") | `main/salida.c:218` (`sanear()`) | translitera á→a … ñ→n; espacio, `-` y `_` → `_`, sin acumular |
| P4 | `main/portal/config_server_viaje.c` (`destino_seguro()`) | misma tabla y misma regla de separadores **desde el 17-oct-2026** |

Ya NO divergen en los separadores (hasta el 17-oct la P4 respetaba el guion:
`Casa-Beach` → `Casa-Beach` en la P4 y `Casa_Beach` en la cabina, y 10 de 21
nombres de una muestra no coincidían). Lo que queda:

1. **La entrada sigue capada a ASCII** en las cabinas
   (`lv_textarea_set_accepted_chars` sobre `s_viaje_destino_ta` y
   `s_destino_nuevo_ta`, en `main/ui/view_registro.c`; la P4 no tiene ese
   fichero: el filtro vive solo en las cabinas). Así que los acentos no llegan a
   la P4 desde la pantalla: el problema está tapado, no resuelto.
2. **El tope no es el mismo**: las cabinas limitan a **20** caracteres lo que se
   teclea (`lv_textarea_set_max_length(..., 20)`); la P4 acepta hasta **23**
   (`DESTINO_MAX 24`, con el cero final).
3. **Con el nombre vacío no hacen lo mismo**: la P4 cae a `"viaje"`; `sanear()`
   devuelve 0 y `salida_abrir_viaje()` se niega a abrir el viaje ("el nombre
   queda vacio al sanearlo"). No crea una carpeta con otro nombre, pero el viaje
   no se abre.

## El criterio (UNO, para las tres placas)

Es el de la cabina desde el 17-oct-2026 (la P4 se cambió a él: es la cabina la que
compone el nombre que ve el usuario y la que manda el destino en el inicio del
viaje). Las cabinas ya cumplen la tabla, los separadores y el tope 20; les falta
el caso del **nombre vacío**. A la P4 le falta el **tope 20**.

1. Se recorre el UTF-8 byte a byte.
2. Si el carácter es `0xC3 0xXX` y `0xXX` está en la tabla, se emite la letra sin
   acento:
   `á é í ó ú Á É Í Ó Ú ñ Ñ ç Ç ü Ü` → `a e i o u A E I O U n N c C u U`.
3. Cualquier otro byte no ASCII (`0x80`-`0xFF`, emoji, comillas tipográficas…) se
   **tira** (no se deja hueco ni se convierte en `_`). Por eso `ï` y `ö` se
   pierden: **no están en la tabla**.
4. Se conservan `a-z A-Z 0-9`.
5. Espacio, `-` y `_` → `_` (el guion **no** se conserva).
6. No se acumulan separadores (`--`, `__`, `-_` → uno solo) y el nombre no puede
   empezar ni terminar en `_`.
7. Tope: **20 caracteres** en las tres placas; si al recortar queda vacío o el
   nombre entero se queda vacío → `viaje`.
8. La función tiene que ser **idempotente**: `f(f(x)) == f(x)` (se aplica al
   recibir y otra vez al componer la ruta).

## Cómo se comprueba (lo que sale HOY)

`sanear()` y `destino_seguro()` se comprueban con la MISMA tabla. La tabla de
verdad de la P4 está en `victron/test/entrada_hostil.sh`, que **extrae y compila
la función real** de `config_server_viaje.c` (no una copia), y en
`victron/test/test_viaje_dueno.sh`, que compila la misma función para las pruebas
de propiedad del viaje. En las cabinas, `test/viaje_*.sh` extraen funciones de
`main/net/viaje_cola.c`, **no** `sanear()`: la tabla de vectores todavía no se
aplica a la cabina.

| entrada | sale |
|---|---|
| `Cádiz` | `Cadiz` |
| `Cataluña` | `Cataluna` |
| `A Coruña-Bilbao` | `A_Coruna_Bilbao` |
| `  doble  espacio  ` | `doble_espacio` |
| `a--b` | `a_b` |
| `ñçü` | `ncu` |
| `   ` | `viaje` (P4) / vacío y no abre (cabina) |
| `./../etc/passwd` | `etcpasswd` |
| `2026-10-11_ruta` | `2026_10_11_ruta` |
| `Bêziers` (ê fuera de tabla) | `Bziers` |
| `Ünïcödé` | `Uncde` (ï y ö no están en la tabla) |
| 24 caracteres | 23 (tope de la P4) |

Lo que este documento pedía antes y **no** da este código (eran vectores
imposibles con la tabla y las reglas de arriba, no bugs):

- `a--b` → `a-b`: el criterio de la cabina convierte el guion en `_`, así que
  sale `a_b`.
- `./../etc/passwd` → `etc_passwd`: las barras y los puntos se **tiran**, no se
  convierten en `_`; sale `etcpasswd` (y el test lo fija así).
- `Ünïcödé` → `Unicode`: `ï` y `ö` no están en la tabla; sale `Uncde`. Si se
  quieren, hay que **añadirlas** a la tabla (y a la copia de la cabina).

## Lo que hay que tocar

- `cabina5/main/salida.c` + `35cabina/main/salida.c`: nada en la tabla ni en los
  separadores (ya coinciden). Decidir si el tope se queda en 20 o sube a 23.
- `cabina5/main/ui/view_registro.c` + `35cabina/main/ui/view_registro.c`:
  levantar el filtro ASCII del destino SOLO cuando el tope y el vacío coincidan
  con la P4 (hoy bloquea los acentos; es lo que evita que el problema se vea).
- `victron/main/portal/config_server_viaje.c`: `destino_seguro()` ya usa la
  tabla y los separadores de la cabina (17-oct-2026); le faltan el **tope 20**
  (hoy 23) y, si se quiere `Unicode`, las letras `ï`/`ö` en la tabla. Si se toca,
  actualizar la copia de las cabinas en el mismo commit (los ficheros de `main/`
  no están en el espejo del CI, así que nada avisará).
