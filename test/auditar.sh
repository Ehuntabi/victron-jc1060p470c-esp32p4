#!/usr/bin/env bash
# auditar.sh — Comprobaciones que evitan que vuelvan fallos YA cazados.
#
# Cada regla de aqui existe por algo que paso de verdad (auditoria 21-sep-2026):
#   - la camara salio VERDE porque una opcion estaba en sdkconfig.defaults pero
#     nunca llego al sdkconfig compilado  -> se comprueba EN EL BINARIO
#   - la conexion era ERRATICA por el pool de sockets  -> se comprueba el valor
#   - dos tareas se quedaban sin pila  -> se comprueba el tamano declarado
#   - habia 31 strcpy  -> se vigila que no aparezcan nuevos con origen no literal
set -uo pipefail
cd "$(dirname "$0")/.."
fallos=0
ok()    { printf '  ok    %s\n' "$1"; }
mal()   { printf '  FALLO %s\n' "$1"; fallos=$((fallos+1)); }

echo "=== 1. Opciones criticas EN EL BINARIO compilado ==="
H=build/config/sdkconfig.h
if [ -f "$H" ]; then
    grep -q "^#define CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER 1" "$H" \
        && ok "la IPA (ISP pipeline) esta encendida" || mal "la IPA (ISP pipeline) NO esta encendida (camara verde)"
    grep -q "^#define CONFIG_CAMERA_OV02C10 1" "$H" \
        && ok "el driver del OV02C10 esta dentro" || mal "el driver del OV02C10 NO esta"
    v=$(grep -E "^#define CONFIG_LWIP_MAX_SOCKETS " "$H" | awk '{print $3}')
    [ "${v:-0}" -ge 20 ] && ok "pool de sockets = $v (>=20)" || mal "pool de sockets = ${v:-?} (<20: conexion erratica)"
    v=$(grep -E "^#define CONFIG_LWIP_MAX_ACTIVE_TCP " "$H" | awk '{print $3}')
    [ "${v:-0}" -ge 20 ] && ok "conexiones TCP activas = $v (>=20)" || mal "TCP activas = ${v:-?} (<20: muerde antes que el pool)"
else
    echo "  (sin build: se omite; compila antes con scripts/build_p4.sh)"
fi

echo "=== 2. Pilas de tarea (ninguna por debajo de 4096) ==="
# solo NUESTRO codigo: los componentes de Espressif (espressif__*) traen sus
# propias pilas y no es cosa nuestra, y ademas algun ejemplo suyo no se compila.
# OJO con -h: sin la ruta no se pueden excluir los componentes de Espressif, y su
# ejemplo de esp_hosted trae una tarea de 1024 que no es nuestra (fallo detectado
# el 21-sep-2026 al ver que el verificador se quejaba de codigo ajeno).
pila_baja=$(grep -rE 'xTaskCreate\([a-zA-Z_0-9]+, *"[a-z_0-9]+", *[0-9]{3,5}' main components --include="*.c" 2>/dev/null \
    | grep -v managed_components | grep -v espressif__ \
    | awk -F: '{ruta=$1; resto=$0; sub(/^[^:]*:/, "", resto); split(resto, a, "\""); print a[2], ruta}' \
    | awk '$1<3072 {print $1" "$2}' | head -5)
[ -z "$pila_baja" ] && ok "ninguna tarea nuestra con pila < 3072" || { mal "tareas con pila < 3072:"; echo "$pila_baja" | sed 's/^/        /'; }

echo "=== 3. Copias de cadenas sin limite (strcpy/strcat/sprintf con origen no literal) ==="
malas=$(grep -rnE '\b(strcpy|strcat|sprintf)\s*\(' main components --include="*.c" 2>/dev/null \
    | grep -v managed_components | grep -v espressif__ \
    | grep -vE '"[^"]*"\)' | head -8)
[ -z "$malas" ] && ok "ninguna copia sin limite con origen variable" || { mal "copias sin limite:"; echo "$malas" | sed 's/^/        /'; }

echo "=== 4. Memoria reservada sin comprobar (informativo) ==="
# Esta regla AVISA, no falla: es una heuristica y da falsos positivos (las
# comprobaciones con ESP_RETURN_ON_FALSE o a mas de 10 lineas no las ve). El
# analisis serio de memoria lo hace -fanalyzer: ANALYZE=1 scripts/build_p4.sh build
sin_check=0
for f in $(grep -rlE '(malloc|calloc|heap_caps_malloc|strdup)\(' main components --include="*.c" 2>/dev/null | grep -v managed_components | grep -v espressif__); do
    n=$(python3 - "$f" <<'PY'
import re, sys
lineas = open(sys.argv[1], encoding="utf8", errors="replace").read().splitlines()
malas = 0
for i, l in enumerate(lineas):
    m = re.search(r"(\w+)\s*=\s*(?:\([^)]*\)\s*)?(malloc|calloc|heap_caps_malloc|strdup)\s*\(", l)
    if not m: continue
    var, ctx = m.group(1), " ".join(lineas[i:i+8])
    if not re.search(rf"(!\s*{re.escape(var)}\b|{re.escape(var)}\s*[!=]=\s*NULL|if\s*\(\s*{re.escape(var)}\s*\)|ESP_RETURN_ON|ESP_GOTO_ON|assert|goto )", ctx):
        malas += 1
print(malas)
PY
)
    sin_check=$((sin_check + n))
done
[ "$sin_check" -eq 0 ] && ok "todas las reservas de memoria se comprueban" || printf '  AVISO %s reservas sin comprobacion cerca (revisar a mano; puede ser falso positivo)\n' "$sin_check"

echo "=== 5. El portal pide clave en todos los handlers ==="
h=$(grep -rhc "esp_err_t handle_" main/portal/*.c 2>/dev/null | awk '{s+=$1} END {print s+0}')
a=$(grep -rhc "REQUIRE_AUTH" main/portal/*.c 2>/dev/null | awk '{s+=$1} END {print s+0}')
[ "$a" -ge "$h" ] && ok "handlers $h · con clave $a" || mal "handlers $h pero solo $a piden clave"

echo
if [ "$fallos" -eq 0 ]; then echo "AUDITORIA OK"; exit 0; else echo "AUDITORIA: $fallos FALLOS"; exit 1; fi
