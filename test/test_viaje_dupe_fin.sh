#!/usr/bin/env bash
# test_viaje_dupe_fin.sh — el 'fin' de un viaje no se escribe dos veces (M4).
#
# El fallo que cubre (auditoria del 27-oct-2026, M4): la unica guarda contra un
# cierre repetido era que existiera resumen.txt, pero la linea 'fin' del diario
# se escribe ANTES que el resumen. Un cierre que se quedo a medias entre las dos
# -- escribir_resumen() no pudo crear su .tmp o la tarjeta se lleno ahi --
# dejaba 'fin' en el eventos.csv y ningun resumen; el reintento de la cola (cada
# 15 s) no veia resumen y volvia a escribir 'fin': 240 lineas en una hora. Y una
# 'fin' de mas puede tapar una fila perdida y que el viaje salga sin la marca de
# INCOMPLETO.
#
# El test NO prueba una copia: extrae diario_ultima_es() del fuente de verdad
# (main/portal/config_server_viaje.c) y la compila con
# test/viaje_dupe_fin_arnes.c. No necesita ESP-IDF ni placa: gcc y ya.
#
# Uso: bash test/test_viaje_dupe_fin.sh
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/fn.c" <<'PY'
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
fn = extrae(v, "static bool diario_ultima_es(const char *carpeta, const char *que)")

plantilla = pathlib.Path("test/viaje_dupe_fin_arnes.c").read_text()
pathlib.Path(sys.argv[1]).write_text(plantilla.replace("__FN__", fn))
PY

gcc -std=gnu11 -Wall -Wextra -Werror -o "$TMP/fn" "$TMP/fn.c"
"$TMP/fn"
