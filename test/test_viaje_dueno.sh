#!/usr/bin/env bash
# Tabla de verdad de la PROPIEDAD DEL VIAJE de la P4 (auditoria del 14-oct-2026).
#
# Lo importante de este test es que NO prueba una copia: extrae las funciones
# REALES de main/portal/config_server_viaje.c (pide_otra_pantalla, byte_de_id,
# byte_de_dev, dev_es_mac, destino_seguro) y las compila con el arnes de
# test/viaje_dueno_arnes.c, que solo sustituye la lectura de la NVS. Asi no
# puede quedarse obsoleto: si alguien cambia la logica, el test cambia con ella.
#
# Lo que fija: quien puede cerrar/apartar un viaje, cuando un apunte se desvia
# al historial del vehiculo, y que la salida de emergencia ('forzar') solo vale
# donde tiene que valer.
set -euo pipefail
cd "$(dirname "$0")/.."

IDF="${IDF_PATH:-}"
for c in "$IDF" "$HOME/.espressif/esp-idf-5.5" /opt/esp/idf; do
    [ -n "$c" ] && [ -f "$c/components/json/cJSON/cJSON.c" ] && IDF="$c" && break
    IDF=""
done
if [ -z "$IDF" ]; then
    echo "SKIP test_viaje_dueno: no encuentro el cJSON del IDF (define IDF_PATH)"
    exit 0
fi
CJ="$IDF/components/json/cJSON"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/piezas.c" <<'PY'
import re, sys, pathlib
def extrae(src, firma):
    m = re.search(re.escape(firma) + r'\s*\n\{', src)
    if not m: raise SystemExit("no encuentro en el fuente: " + firma)
    i = m.start(); k = src.index('{', m.start()); n = 0
    while True:
        if src[k] == '{': n += 1
        elif src[k] == '}':
            n -= 1
            if n == 0: break
        k += 1
    return src[i:k+1]
v  = pathlib.Path('main/portal/config_server_viaje.c').read_text()
ca = pathlib.Path('main/portal/contrato_app.c').read_text()
piezas = [
    extrae(v, 'static bool es_digito_hex(char c)'),
    extrae(v, 'static bool dev_es_mac(const char *dev)'),
    extrae(v, 'static int byte_de_id(uint32_t id)'),
    extrae(v, 'static int byte_de_dev(const char *dev)'),
    extrae(v, 'static bool pide_otra_pantalla(const cJSON *j, uint32_t id, bool admite_forzar)'),
    extrae(v, 'static void destino_seguro(const char *in, char *out, size_t n)'),
    extrae(ca, 'int viaje_dev_ajeno(const char *dev_pide, const char *dev_abierto)'),
]
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(piezas) + '\n')
PY

python3 - "$TMP/test.c" "$TMP/piezas.c" <<'PY'
import sys, pathlib
arnes = pathlib.Path('test/viaje_dueno_arnes.c').read_text()
piezas = pathlib.Path(sys.argv[2]).read_text()
pathlib.Path(sys.argv[1]).write_text(arnes.replace('/*__PIEZAS__*/', piezas))
PY

gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -o "$TMP/test_viaje_dueno" \
    "$TMP/test.c" "$CJ/cJSON.c" -I"$CJ" -lm
"$TMP/test_viaje_dueno"
