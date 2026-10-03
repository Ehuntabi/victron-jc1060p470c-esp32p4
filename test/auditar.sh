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
# OJO con el parseo: la version anterior imprimia el NOMBRE de la tarea y lo
# comparaba con 3072, y en awk una cadena vale 0 -> "0 < 3072" -> nunca cazaba
# nada (fallo silencioso detectado el 3-oct-2026 metiendo una tarea de 1024 a
# proposito). Ahora se extrae el numero con sed y se compara el numero.
pila_baja=$(grep -rE 'xTaskCreate\([a-zA-Z_0-9]+, *"[a-z_0-9]+", *[0-9]{3,5}' main components --include="*.c" 2>/dev/null \
    | grep -v managed_components | grep -v espressif__ \
    | sed -E 's/^([^:]+):.*"([a-z_0-9]+)", *([0-9]+).*/\3 \2 \1/' \
    | awk '$1+0 < 3072 {print "        " $2 " (" $1 ") en " $3}' | head -5)
[ -z "$pila_baja" ] && ok "ninguna tarea nuestra con pila < 3072" || { mal "tareas con pila < 3072:"; echo "$pila_baja" | sed 's/^/        /'; }

echo "=== 2b. LVGL desde tareas: siempre bajo bsp_display_lock ==="
# Medido a golpes el 2-oct-2026 en el propio banco de pruebas: llamar a LVGL desde
# una tarea que no es la de LVGL sin el lock acaba en panic (get_prop_core,
# lv_obj_style.c). Los ficheros con tareas que tocan LVGL tienen que usar
# bsp_display_lock/lvgl_port_lock. Las llamadas dentro de comentarios NO cuentan.
# La regla mira el FICHERO, no el camino de llamada, asi que hay excepciones
# revisadas a mano (una por linea, con el porque):
#   main/alarma_estado.c: crea alarma_task (el pitido) y esa tarea NO toca LVGL
#     (0 llamadas). Sus lv_ estan en aviso_crear() y en tick_cb(), que es un
#     callback de lv_timer y por tanto corre DENTRO de la tarea de LVGL.
#     Revisado el 3-oct-2026.
EXCEPCIONES_LVGL=" main/alarma_estado.c "
sospechosos=""
for f in $(grep -rlE 'xTaskCreate\(' main components --include="*.c" 2>/dev/null | grep -v managed_components | grep -v espressif__); do
    case "$EXCEPCIONES_LVGL" in *" $f "*) continue;; esac
    nlv=$(grep -vE '^[[:space:]]*(/\*|\*|//)' "$f" | grep -cE '\blv_[a-z_]+\(' || true)
    nlock=$(grep -cE 'bsp_display_lock|lvgl_port_lock' "$f" || true)
    [ "$nlv" -gt 0 ] && [ "$nlock" -eq 0 ] && sospechosos="$sospechosos $f"
done
[ -z "$sospechosos" ] && ok "ningun fichero con tareas toca LVGL sin cerrojo" \
    || mal "tocan LVGL sin bsp_display_lock:$sospechosos"

echo "=== 2c. El tick del sistema a 1000 Hz ==="
# esp_hosted avisa en CADA arranque de que recomienda 1000 ("to avoid bus level
# jitters") y se midio el efecto el 3-oct-2026, mismo banco y tarjeta con la
# radio levantada: la lectura por FATFS pasaba de 0,33 MB/s a 1,33 MB/s (4x) solo
# por el tick, porque cada espera de transaccion SDMMC tiene granularidad de un
# tick (10 ms con 100 Hz). Antes de subirlo se comprobo que no hay ni una espera
# en ticks literales en el codigo propio: todo va con pdMS_TO_TICKS.
if [ -f "$H" ]; then
    v=$(grep -E "^#define CONFIG_FREERTOS_HZ " "$H" | awk '{print $3}')
    [ "${v:-0}" -ge 1000 ] && ok "tick del sistema = ${v} Hz" \
        || mal "CONFIG_FREERTOS_HZ=${v:-?}: con 100 Hz la lectura de la SD pierde 4x (y esp_hosted avisa)"
fi

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
    # El lvalue entero, no solo la ultima palabra: `s->x = malloc(...)` se
    # comprueba con `if (!s->x)`, y buscar solo "x" no lo veia (7 falsos
    # positivos el 3-oct-2026, todos ellos con la comprobacion a la vista).
    m = re.search(r"([\w\->\.\[\]]+)\s*=\s*(?:\([^)]*\)\s*)?(malloc|calloc|heap_caps_malloc|strdup)\s*\(", l)
    if not m: continue
    var, ctx = m.group(1).strip(), " ".join(lineas[i:i+10])
    esc = re.escape(var)
    # Sin \b final si acaba en ']': ahi el limite de palabra nunca casa.
    fin = "" if var.endswith("]") else r"\b"
    if not re.search(rf"(!\s*{esc}{fin}|{esc}\s*[!=]=\s*NULL|if\s*\(\s*{esc}\s*\)|ESP_RETURN_ON|ESP_GOTO_ON|assert|goto )", ctx):
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

echo "=== 6. La SD: transporte, reloj y el C6 mientras se identifica ==="
# Cada regla existe por algo medido el 2-oct-2026:
#   - con la SD en SPI a 20 MHz la lectura cruda era 14x mas lenta y la escritura
#     se quedaba en 0,13 MB/s (inviable para fotos/video)
#   - con el C6 VIVO de la sesion anterior (un reinicio de la P4 no lo resetea) el
#     montaje fallaba 3 de 3 intentos con ESP_ERR_TIMEOUT en ACMD41; con el C6 en
#     reset durante la identificacion monta a la primera (6/6 y 23 arranques)
DL=components/datalogger/datalogger.c
grep -qE 'host\.slot[[:space:]]*=[[:space:]]*SDMMC_HOST_SLOT_0' "$DL" \
    && ok "la SD va por SDMMC en el slot 0" || mal "la SD no usa SDMMC_HOST_SLOT_0"
grep -qE 'max_freq_khz[[:space:]]*=[[:space:]]*SDMMC_FREQ_HIGHSPEED' "$DL" \
    && ok "la SD pide 40 MHz (el mismo divisor que el enlace del C6)" \
    || mal "la SD no pide SDMMC_FREQ_HIGHSPEED: a 20 MHz el reloj del C6 se queda a la mitad"
grep -q "slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP" "$DL" \
    && ok "pull-ups internos en las lineas de la SD" || mal "sin SDMMC_SLOT_FLAG_INTERNAL_PULLUP"
grep -q "slot_config.width = (i < 2) ? 4 : 1" "$DL" \
    && ok "bus de 4 bits (con 1 bit de reserva en el 3er intento)" || mal "no monta a 4 bits"
grep -q "c6_en_reset(true)" "$DL" && grep -q "c6_en_reset(false)" "$DL" \
    && ok "C6 en reset mientras se identifica la SD, y suelto despues" \
    || mal "falta el C6 en reset (o soltarlo) durante la identificacion de la SD"

echo "=== 7. Lectura de la SD: bufer de stdio y lectores de historicos ==="
# Medido el 2-oct-2026: con CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0 (512 B en la
# practica) cada fread/fgets pedia UN SECTOR por transaccion y la lectura por
# fichero se quedaba en 0,30 MB/s. Con 16 KB sube a 2,0 MB/s (fread) y abre un dia
# de bateria en ~1 s. Y los historicos se leen por bloques (no fgets) porque el
# recorrido linea a linea costaba 5x mas.
if [ -f "$H" ]; then
    v=$(grep -E "^#define CONFIG_FATFS_VFS_FSTAT_BLKSIZE " "$H" | awk '{print $3}')
    [ "${v:-0}" -ge 8192 ] && ok "bufer de stdio de la SD = ${v} B (>=8192)" \
        || mal "CONFIG_FATFS_VFS_FSTAT_BLKSIZE=${v:-?}: la lectura de ficheros vuelve a ir sector a sector"
fi
LB=main/log_browser.c
grep -qE 'lb_linea\(' "$LB" && grep -qE 'lb_abrir\(' "$LB" \
    && ok "los historicos se leen por bloques (lb_abrir/lb_linea)" \
    || mal "log_browser.c no lee por bloques"
grep -vE '^[[:space:]]*(/\*|\*|//)' "$LB" | grep -qE '\b(fgets|sscanf)[[:space:]]*\(' \
    && mal "log_browser.c ha vuelto a fgets/sscanf por linea (5x mas lento)" \
    || ok "sin fgets ni sscanf en el camino de lectura"
for f in components/datalogger/datalogger.c components/battery_history/battery_history.c main/ne185_vlog.c; do
    grep -qE 'fclose\(.*\)[[:space:]]*!=[[:space:]]*0' "$f" \
        && ok "comprueba el fclose: $(basename $f)" \
        || mal "no comprueba el fclose (perdida silenciosa de muestras): $f"
done
grep -q "unlink(path)" main/screenshot.c \
    && ok "la captura a medias se borra si falla la escritura" \
    || mal "screenshot.c deja JPEG truncados en la tarjeta"

echo "=== 8. La auditoria no puede estar mirando un BUILD VIEJO ==="
# Si el build es anterior al ultimo commit de codigo, todo lo de arriba habla de
# un firmware que ya no existe. Paso el 2-oct-2026: se auditaba build/ con horas
# de retraso respecto al codigo.
if [ -f "$H" ]; then
    ultimo=$(git log -1 --format=%ct -- sdkconfig main components 2>/dev/null || echo 0)
    mtime=$(stat -c %Y "$H" 2>/dev/null || echo 0)
    if [ "$ultimo" -gt 0 ] && [ "$mtime" -lt "$ultimo" ]; then
        mal "el build es MAS VIEJO que el ultimo commit de codigo: recompila (scripts/build_p4.sh o release.sh) antes de auditar"
    else
        ok "el build es posterior al ultimo commit de codigo"
    fi
fi

echo "=== 9. mini_proto.h igual en los dos repos (si esta al lado) ==="
# El 2-oct-2026 el CI de la cabina fallo porque se publico la cabina 70 s antes
# que el cambio del P4: el protocolo se sincroniza A MANO.
if [ -f ../35cabina/main/net/mini_proto.h ]; then
    diff -q main/net/mini_proto.h ../35cabina/main/net/mini_proto.h >/dev/null 2>&1 \
        && ok "mini_proto.h identico en victron y 35cabina" \
        || mal "mini_proto.h DISTINTO entre los dos repos (sincronizar a mano)"
else
    echo "  (sin el repo de la cabina al lado: se omite)"
fi

echo "=== 10. dependencies.lock sin rutas de esta maquina ==="
# Un build local reescribe dependencies.lock con la ruta ABSOLUTA del espejo
# (~/.scratch/victron) y con eso el CI de GitHub falla: la entrada de esp_hosted
# tiene que quedar RELATIVA (components/espressif__esp_hosted). Leccion del
# CLAUDE.md del proyecto; los scripts de build ya lo revierten, esto lo vigila.
# Lo que rompe el CI es que se COMMITEE con ruta absoluta. El fichero de trabajo
# la lleva despues de cada build local (lo reescribe idf.py), y eso es normal:
# los scripts de build lo revierten antes de commitear.
if git show HEAD:dependencies.lock >/dev/null 2>&1; then
    if git show HEAD:dependencies.lock | grep -qE "$HOME|/home/"; then
        mal "el dependencies.lock COMMITEADO lleva una ruta absoluta: el CI de GitHub fallara (usa rutas relativas)"
    else
        ok "el dependencies.lock commiteado solo tiene rutas relativas"
        grep -qE "$HOME|/home/" dependencies.lock 2>/dev/null \
            && echo "  (el de trabajo si lleva churn del ultimo build: git checkout -- dependencies.lock antes de commitear)"
    fi
fi

echo
if [ "$fallos" -eq 0 ]; then echo "AUDITORIA OK"; exit 0; else echo "AUDITORIA: $fallos FALLOS"; exit 1; fi
