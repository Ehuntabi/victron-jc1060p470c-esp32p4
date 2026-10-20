#!/usr/bin/env bash
# Tabla de verdad del TOKEN DE LAS PANTALLAS (tanda 1, 20-oct-2026).
#
# Extrae las funciones REALES de main/portal/config_server_viaje.c (token_
# normaliza, token_de_pantalla_clave, token_autoriza_forzar, dev_es_mac,
# es_digito_hex) y de main/portal/config_server_auth.c (auth_equal_ct) y las
# compila con test/viaje_token_arnes.c. No prueba una copia: si alguien cambia la
# logica, este test cambia con ella.
#
# NO necesita IDF (no usa cJSON) ni placa: gcc y ya. Por eso puede correr en el
# CI sin el arbol de ESP-IDF.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/piezas.c" <<'PY'
import re, sys, pathlib

def extrae(src, firma):
    """Extrae la funcion ENTERA por su firma (que puede venir partida en varias
    lineas). Las llaves se cuentan saltando cadenas y comentarios."""
    patron = r"\s*".join(re.escape(t) for t in firma.split()) + r"\s*\n\{"
    m = re.search(patron, src)
    if not m:
        raise SystemExit("no encuentro en el fuente: " + firma)
    i = m.start(); k = src.index('{', m.start()); n = 0
    while k < len(src):
        c = src[k]
        if c == '/' and src[k+1:k+2] == '/':
            k = src.find('\n', k); k = len(src) if k < 0 else k
            continue
        if c == '/' and src[k+1:k+2] == '*':
            k = src.find('*/', k); k = len(src) if k < 0 else k + 2
            continue
        if c in ('"', "'"):
            q = c; k += 1
            while k < len(src) and src[k] != q:
                k += 2 if src[k] == '\\' else 1
            k += 1
            continue
        if c == '{': n += 1
        elif c == '}':
            n -= 1
            if n == 0: return src[i:k+1]
        k += 1
    raise SystemExit("llaves sin cerrar extrayendo: " + firma)

v = pathlib.Path('main/portal/config_server_viaje.c').read_text()
a = pathlib.Path('main/portal/config_server_auth.c').read_text()
piezas = [
    extrae(v, 'static bool es_digito_hex(char c)'),
    extrae(v, 'static bool dev_es_mac(const char *dev)'),
    extrae(v, 'static bool token_normaliza(const char *t, char *out, size_t n)'),
    extrae(v, 'static void token_de_pantalla_clave(const char *dev, char *out, size_t n)'),
    extrae(v, 'static bool token_autoriza_forzar(const char *dev, const char *token_cuerpo, const char *token_guardado)'),
    extrae(a, 'bool auth_equal_ct(const char *a, const char *b)'),
]
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(piezas) + '\n')
PY

python3 - "$TMP/test.c" "$TMP/piezas.c" <<'PY'
import sys, pathlib
arnes = pathlib.Path('test/viaje_token_arnes.c').read_text()
piezas = pathlib.Path(sys.argv[2]).read_text()
pathlib.Path(sys.argv[1]).write_text(arnes.replace('/*__PIEZAS__*/', piezas))
PY

CC="${CC:-gcc}"
"$CC" -std=gnu17 -Wall -Wextra -Werror -O1 -o "$TMP/test_viaje_token" "$TMP/test.c"
"$TMP/test_viaje_token"
