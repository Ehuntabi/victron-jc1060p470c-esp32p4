#!/usr/bin/env bash
# test_viaje_cierre.sh — el CIERRE del viaje tiene que ser IDEMPOTENTE (D0).
#
# El fallo que cubre (17-oct-2026): resumen.txt es lo que marca un viaje como
# cerrado, y el ultimo paso del cierre era un rename() del .tmp. En FatFs ese
# rename FALLA si el destino ya existe (la POSIX lo pisa), asi que en cuanto
# quedaba un resumen.txt de un intento anterior -- o lo creaba el bloque de
# "faltan apuntes", que iba antes del return --, TODOS los reintentos del fin
# fallaban y el viaje se quedaba sin poder cerrar PARA SIEMPRE: ni la cabina, ni
# la web (mismo op_fin), ni el .tar (409 por viaje_es_el_abierto).
#
# El test NO prueba una copia: extrae publicar_resumen() del fuente de verdad y
# la compila con test/viaje_cierre_arnes.c, que sustituye rename() por uno que
# se comporta como el de FatFs. No necesita ESP-IDF ni placa: gcc y ya.
#
# Uso: bash test/test_viaje_cierre.sh
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/piezas.c" <<'PY'
import re, sys, pathlib
COMILLA = chr(39)

def extrae(src, firma):
    """Extrae la funcion entera por su firma, contando llaves con un mini-lexer
    que salta cadenas y comentarios (si no, las llaves de un literal descuadran
    la cuenta y se lleva medio fichero)."""
    patron = r"\s*".join(re.escape(t) for t in firma.split()) + r"\s*\n\{"
    m = re.search(patron, src)
    if not m: raise SystemExit("no encuentro en el fuente: " + firma)
    i = m.start(); k = src.index("{", m.start()); n = 0
    while k < len(src):
        c = src[k]
        if c == "/" and src[k+1:k+2] == "/":
            k = src.find(chr(10), k); k = len(src) if k < 0 else k
            continue
        if c == "/" and src[k+1:k+2] == "*":
            k = src.find("*/", k); k = len(src) if k < 0 else k + 2
            continue
        if c == chr(34) or c == COMILLA:
            q = c; k += 1
            while k < len(src) and src[k] != q:
                k += 2 if src[k] == chr(92) else 1
            k += 1
            continue
        if c == "{": n += 1
        elif c == "}":
            n -= 1
            if n == 0: return src[i:k+1]
        k += 1
    raise SystemExit("llaves sin cerrar extrayendo: " + firma)

v = pathlib.Path("main/portal/config_server_viaje.c").read_text()
piezas = [
    extrae(v, "static bool publicar_resumen(const char *ruta_tmp, const char *ruta)"),
]
pathlib.Path(sys.argv[1]).write_text("\n\n".join(piezas) + "\n")
PY

# Las medidas, leidas de su fuente (no copiadas a mano): RUTA_MAX es el tamano
# del buffer del .bak dentro de la funcion.
{ grep -E "^#define (CARPETA_MAX|RUTA_MAX|DESTINO_MAX) " \
      main/portal/config_server_viaje.c || true; } > "$TMP/medidas.h"

python3 - "$TMP/test.c" "$TMP/piezas.c" "$TMP/medidas.h" <<'PY'
import sys, pathlib
arnes  = pathlib.Path('test/viaje_cierre_arnes.c').read_text()
piezas = pathlib.Path(sys.argv[2]).read_text()
# Las medidas se meten ANTES de las piezas (la funcion usa RUTA_MAX).
arnes = arnes.replace('#include "medidas.h"\n', '')
arnes = arnes.replace('/*__PIEZAS__*/',
                      pathlib.Path(sys.argv[3]).read_text() + '\n' + piezas)
pathlib.Path(sys.argv[1]).write_text(arnes)
PY

gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -o "$TMP/test_viaje_cierre" "$TMP/test.c"
"$TMP/test_viaje_cierre"
