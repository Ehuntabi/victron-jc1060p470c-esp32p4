#!/usr/bin/env bash
# test_pantalla_fw.sh — prueba SIN PLACA la validacion del .bin que se sube
# para las pantallas (POST /api/pantalla/subir).
#
# POR QUE EXISTE: un .bin que no sea de la pantalla pedida (o que este roto)
# no se puede publicar, porque la cabina se lo baja y se queda sin arrancar.
# La validacion de verdad esta en main/portal/ota_pantalla.c; aqui se EXTRAE
# esa funcion del fuente y se compila con gcc, igual que test_viaje_dueno.sh
# hace con las reglas del viaje. No es una copia: si alguien cambia la funcion,
# el test cambia con ella.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=main/portal/ota_pantalla.c
[ -f "$SRC" ] || { echo "FALLO: no encuentro $SRC"; exit 1; }

# El tope (0x3F0000) y la cabecera (0x70) son parte del contrato. Si alguien
# los cambia sin querer, esto lo dice aunque la funcion siga compilando.
grep -qE '^#define PANTALLA_MAX_BYTES[[:space:]]+0x3F0000u' "$SRC" \
    || { echo "FALLO: PANTALLA_MAX_BYTES ya no es 0x3F0000 en $SRC"; exit 1; }
grep -qE '^#define PANTALLA_CABECERA[[:space:]]+0x70u' "$SRC" \
    || { echo "FALLO: PANTALLA_CABECERA ya no es 0x70 en $SRC"; exit 1; }
grep -q "pantalla_fw_valido(cab" "$SRC" \
    || { echo "FALLO: el handler de subida no llama a pantalla_fw_valido()"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$SRC" "$TMP/piezas.c" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1]).read_text()

macro = re.search(r'^#define[ \t]+PANTALLA_CABECERA[ \t]+.*$', src, re.M)
if not macro:
    raise SystemExit('no encuentro #define PANTALLA_CABECERA en ' + sys.argv[1])

m = re.search(r'^static bool pantalla_fw_valido[ \t]*\(', src, re.M)
if not m:
    raise SystemExit('no encuentro pantalla_fw_valido() en ' + sys.argv[1])

i = m.start()
k = src.index('{', i)
depth = 0
while k < len(src):
    c = src[k]
    if c == '{':
        depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0:
            k += 1
            break
    k += 1
if depth != 0:
    raise SystemExit('no he podido delimitar el cuerpo de pantalla_fw_valido()')

pathlib.Path(sys.argv[2]).write_text(macro.group(0) + '\n\n' + src[i:k] + '\n')
PY

python3 - "$TMP/piezas.c" "$TMP/test.c" <<'PY'
import pathlib
import sys

arnes = pathlib.Path('test/test_pantalla_fw_arnes.c').read_text()
piezas = pathlib.Path(sys.argv[1]).read_text()
marca = '/*__PIEZAS__*/'
if marca not in arnes:
    raise SystemExit('el arnes no tiene la marca ' + marca)
pathlib.Path(sys.argv[2]).write_text(arnes.replace(marca, piezas, 1))
PY

gcc -std=gnu17 -Wall -Wextra -Werror -O1 "$TMP/test.c" -o "$TMP/test_pantalla_fw"
"$TMP/test_pantalla_fw"
