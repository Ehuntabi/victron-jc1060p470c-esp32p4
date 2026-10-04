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

echo "=== 2b. LVGL siempre bajo cerrojo ==="
# Medido a golpes el 2-oct-2026: llamar a LVGL sin el lock acaba en panic
# (get_prop_core, lv_obj_style.c). Se vigilan los dos casos que de verdad
# pueden pasar:
#   (A) un fichero que CREA una tarea y ademas toca LVGL
#   (B) un fichero FUERA de main/ui/ que toca LVGL (handlers del httpd, capturas,
#       splash, red...): el codigo de main/ui/ es la capa de UI y se ejecuta en la
#       tarea de LVGL, asi que ahi el lock no hace falta (y pedirlo seria ruido).
# Excepciones revisadas a mano, cada una con su porque:
#   main/alarma_estado.c: crea alarma_task (el pitido) y esa tarea NO toca LVGL
#     (0 llamadas). Sus lv_ estan en aviso_crear() y en tick_cb(), callback de
#     lv_timer que corre DENTRO de la tarea de LVGL.
#   main/splash.c: sus 35 llamadas se hacen desde splash_show()/splash_hide(), y
#     los DOS sitios que las llaman (main.c) estan dentro de lvgl_port_lock(0).
#     Comprobado leyendo los dos sitios, no el comentario.
#   main/ui.c: es el codigo de la propia tarea de LVGL.
EXCEPCIONES_LVGL=" main/alarma_estado.c main/splash.c main/ui.c main/lv_port.c main/esp_bsp.c "
sospechosos=""
for f in $(grep -rlE '\blv_[a-z_]+\(' main components --include="*.c" 2>/dev/null | grep -v managed_components | grep -v espressif__); do
    case "$EXCEPCIONES_LVGL" in *" $f "*) continue;; esac
    nlv=$(grep -vE '^[[:space:]]*(/\*|\*|//)' "$f" | grep -cE '\blv_[a-z_]+\(' || true)
    [ "$nlv" -eq 0 ] && continue
    nlock=$(grep -cE 'bsp_display_lock|lvgl_port_lock' "$f" || true)
    [ "$nlock" -gt 0 ] && continue
    ntask=$(grep -cE 'xTaskCreate\(' "$f" || true)
    case "$f" in
        main/ui/*) [ "$ntask" -gt 0 ] && sospechosos="$sospechosos $f" ;;   # (A)
        *)         sospechosos="$sospechosos $f" ;;                          # (B)
    esac
done
[ -z "$sospechosos" ] && ok "LVGL siempre bajo cerrojo (o en la lista revisada)" \
    || mal "tocan LVGL sin bsp_display_lock:$sospechosos"

echo "=== 2d. La OTA: que un firmware malo no deje la placa inservible ==="
# La OTA es el flujo mas peligroso que hay (se actualiza el van desde el portal).
# Cada regla protege un paso concreto de la red de seguridad.
H2="build/config/sdkconfig.h"
if [ -f "$H2" ]; then
    grep -q "^#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1" "$H2" \
        && ok "rollback del bootloader encendido (una OTA que no arranca se revierte)" \
        || mal "rollback APAGADO: una OTA que no arranque deja la placa sin volver atras"
fi
grep -q "esp_ota_mark_app_valid_cancel_rollback" main/main.c \
    && ok "la app marca el arranque como valido (si no, el rollback se dispara solo)" \
    || mal "la app NO marca el arranque valido: con rollback encendido se reiniciaria en bucle"
OTA=main/portal/ota_update.c
if [ -f "$OTA" ]; then
    grep -qE 'content_len > destino->size' "$OTA" \
        && ok "el OTA rechaza imagenes mayores que la particion (el -full de 9 MB no entra)" \
        || mal "el OTA no comprueba el tamano contra la particion"
    grep -q "esp_ota_end(" "$OTA" && ok "valida la imagen antes de aceptarla (esp_ota_end)" \
        || mal "el OTA no llama a esp_ota_end: aceptaria una imagen corrupta"
    grep -q "esp_ota_set_boot_partition" "$OTA" && ok "fija la particion de arranque" \
        || mal "el OTA no fija la particion de arranque"
    grep -q "esp_ota_abort" "$OTA" && ok "aborta limpio si se corta la subida" \
        || mal "el OTA no aborta si se corta la subida"
fi
grep -q "REQUIRE_AUTH_STRICT(req)" main/portal/config_server.c \
    && ok "el POST de /ota pide clave (estricta)" \
    || mal "el POST de /ota NO pide clave: cualquiera en el AP podria flashear la placa"
# Sin dos huecos de app y su otadata no hay OTA posible (y un partitions.csv mal
# tocado deja la placa sin poder actualizarse).
grep -qE "^ota_0," partitions.csv && grep -qE "^ota_1," partitions.csv \
    && ok "las dos particiones de app (ota_0/ota_1) existen" \
    || mal "falta ota_0 u ota_1 en partitions.csv: sin los dos huecos no hay OTA"
grep -qE "^otadata," partitions.csv && ok "particion otadata presente" \
    || mal "sin otadata el bootloader no sabe que hueco arrancar"

echo "=== 2e. El watchdog vigila las tareas del enlace con la cabina ==="
# Anadidas el 3-oct-2026, y cada una por un motivo distinto:
#   - udp_latido: si se cuelga, la P4 cree que la cabina se ha muerto (deja de
#     llegar su latido) y empieza a reiniciar su propio AP en cascada.
#   - udp_tx: si se cuelga, la cabina deja de recibir telemetria en silencio.
# Ademas el log del reset tiene que decir el NOMBRE de la tarea, no su numero:
# en el campo "Tarea 7 sin latido" no sirve de nada.
grep -q "WD_TASK_UDP_TX" main/watchdog.h && grep -q "WD_TASK_UDP_LATIDO" main/watchdog.h \
    && ok "las dos tareas del enlace estan en la tabla del watchdog" \
    || mal "falta alguna tarea del enlace en el watchdog (ver watchdog.h)"
grep -q "watchdog_heartbeat(WD_TASK_UDP_TX)" main/net/udp_tx.c \
    && ok "udp_tx late" || mal "udp_tx NO late: su cuelgue no lo veria nadie"
grep -q "watchdog_heartbeat(WD_TASK_UDP_LATIDO)" main/net/udp_latido.c \
    && ok "udp_latido late" || mal "udp_latido NO late: su cuelgue pareceria un problema de radio"
grep -q "WD_TASK_NAMES" main/watchdog.c \
    && ok "el reset controlado dice el nombre de la tarea" \
    || mal "el reset dice el numero de tarea en vez del nombre"
# El monitor ignora a proposito las entradas con s_last_beat == 0 (nunca latio).
# Sin el aviso, una tarea que no llega a arrancar no se vigila NI se dice: hueco
# silencioso cerrado el 3-oct-2026. El plazo tiene que ser 2x el de ESA entrada
# (con un plazo global saltaba con tareas sanas que aun no habian latido: visto).
grep -q "NO ha latido nunca" main/watchdog.c && grep -q "2 \* WD_TASK_TIMEOUT_US_TABLE" main/watchdog.c \
    && ok "avisa de las entradas que nunca han latido (2x su propio plazo)" \
    || mal "falta el aviso de las entradas que nunca laten (hueco silencioso del watchdog)"

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
# El reintento tiene que cortar la corriente MAS que el arranque: medido el
# 2-oct-2026, con la tarjeta en mal estado fallaron los 3 intentos (send_scr
# 0x107) y cada reintento volvia a cortar 300 ms. Repetir el mismo corte no
# aporta nada; 1500 ms dan tiempo al reset del controlador de la tarjeta.
a=$(grep -oE '#define SD_CORTE_ARRANQUE_MS +[0-9]+' "$DL" | grep -oE '[0-9]+')
b=$(grep -oE '#define SD_CORTE_REINTENTO_MS +[0-9]+' "$DL" | grep -oE '[0-9]+')
if [ -n "$a" ] && [ -n "$b" ]; then
    [ "$b" -gt "$a" ] && ok "el reintento corta la corriente mas que el arranque ($b > $a ms)" \
        || mal "el reintento corta lo mismo o menos que el arranque ($b <= $a ms)"
else
    mal "no encuentro SD_CORTE_ARRANQUE_MS/SD_CORTE_REINTENTO_MS en datalogger.c"
fi

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

echo "=== 11. La camara: los ajustes que costaron medirlos (3-oct-2026) ==="
# Cada regla de aqui viene de un fallo MEDIDO con fotos reales del equipo, no de
# una preferencia. Si alguien los revierte sin medir, esto lo dice.
CAMJSON=components/ov02c10/cfg/ov02c10_default.json
# 1) Objetivo de brillo del AE: en 55 (el valor que traia) la cara salia muy
#    oscura (luma del parche de la cara 76 -> 137 al subirlo a 100). Por encima de
#    110 vuelve a quemar el canal rojo, porque el AWB lo levanta.
TARGET=$(python3 -c "import json;print(json.load(open('$CAMJSON'))['OV02C10']['agc']['luma_adjust']['target'])" 2>/dev/null)
if [ -n "$TARGET" ] && [ "$TARGET" -ge 70 ] && [ "$TARGET" -le 110 ]; then
    ok "objetivo de brillo del AE en rango util ($TARGET, entre 70 y 110)"
else
    mal "objetivo de brillo del AE fuera de rango ($TARGET): con 55 la cara sale oscura y por encima de 110 se satura el rojo"
fi
# 2) Ventana de puntos blancos del AWB, CENTRADA en el neutro real del sensor
#    (rg 0,53 / bg 0,64, medido el 3-oct-2026 con el techo blanco de referencia).
#    Historia de este numero, que costo tres vueltas:
#      - 0,573-0,9096 (heredada del SC2336): excluia el neutro real -> MAGENTA de
#        noche.
#      - 0,35-0,80 / 0,40-0,90 (centrada): bien de noche.
#      - 0,2-1,6 (v4.8, "abrirla a los pixeles calidos"): arreglo el rojo de dia
#        pero devolvio el magenta de noche -- medido el 4-oct-2026 en el mismo
#        salon: el TECHO BLANCO salia R/G=1,99 (magenta) y con la ventana centrada
#        sale R/G=0,98-1,03 en las dos luces (lampara calida y luz blanca fuerte).
#    O sea: ancha NO es mejor. Lo que arreglaba el rojo de dia era el objetivo del
#    AE (55 -> 100), no ensanchar la ventana.
read -r RGMIN RGMAX BGMIN BGMAX <<EOF
$(python3 -c "
import json;a=json.load(open('$CAMJSON'))['OV02C10']['awb']['range']
print(a['rg']['min'], a['rg']['max'], a['bg']['min'], a['bg']['max'])" 2>/dev/null)
EOF
if python3 -c "
import sys
rgmin,rgmax,bgmin,bgmax = float('$RGMIN'),float('$RGMAX'),float('$BGMIN'),float('$BGMAX')
# tiene que CONTENER el neutro real (0,53/0,64) y no ser un colador (0,2-1,6)
sys.exit(0 if rgmin>=0.3 and rgmin<=0.53 and rgmax>=0.64 and rgmax<=1.0
           and bgmin>=0.35 and bgmin<=0.64 and bgmax>=0.64 and bgmax<=1.1 else 1)
" 2>/dev/null; then
    ok "ventana del AWB centrada en el neutro real (R/G $RGMIN-$RGMAX, B/G $BGMIN-$BGMAX)"
else
    mal "ventana del AWB descentrada o demasiado ancha (R/G $RGMIN-$RGMAX, B/G $BGMIN-$BGMAX): si excluye el neutro (0,53/0,64) sale magenta, y si lo acepta TODO tambien (medido: techo R/G 1,99)"
fi
# 2b) El AE tiene que proteger las SOMBRAS, no las luces altas: la camara mira a
#     contraluz (lampara detras de la cabeza) y con high_light_priority el AE
#     daba 5x de peso a las zonas brillantes -> CARA EN Y=46 mientras la lampara
#     estaba en 215. Con low_light_priority: cara Y=93 y la lampara 159 (mismo
#     salon, misma luz, 4-oct-2026).
# 3) La foto pedida con la camara en reposo tiene que esperar a que la imagen este
#    asentada: sin eso se servia a mitad de convergencia del AE/AWB y con la MISMA
#    configuracion salia una foto buena y otra roja.
grep -q "firma_miniatura" components/camera/camera.c && grep -q "FOTO_ESPERA_ITER" components/camera/camera.c \
    && ok "la foto espera a que la imagen este asentada" \
    || mal "la foto se sirve sin esperar a que el AE/AWB asienten (salia buena o roja al azar)"
# 4) Recuperacion del sensor atascado POR SOFTWARE (4-oct-2026). Lo que cuesta
#    medir es el ORDEN: la tabla se escribe con el sensor PARADO (si no, la imagen
#    sale magenta y no vuelve), se respeta REG_END/DELAY como el driver, y despues
#    se reinicia la placa porque el AWB del ISP del P4 se queda desbocado.
OV=components/ov02c10/ov02c10.c
CAM=components/camera/camera.c
if grep -q "stream OFF" $OV && grep -q "OV02C10_REG_END" $OV && grep -q "OV02C10_REG_DELAY" $OV; then
    ok "la recuperacion del sensor para el stream y respeta REG_END/DELAY"
else
    mal "la recuperacion escribe la tabla con el sensor emitiendo o se salta REG_END/DELAY (sale magenta y no vuelve)"
fi
if grep -q "ov02c10_recover_over_i2c" $CAM && grep -q "s_cam_pausa = true" $CAM; then
    ok "el bucle de captura se PAUSA antes de tocar el sensor"
else
    mal "se toca el sensor con el bucle corriendo: el AE del ISP escribe a la vez y el sensor deja de responder (PID=0x0)"
fi
if grep -q "esp_restart()" $CAM && grep -q "CAM_REINICIOS_MAX" $CAM && grep -q "RTC_NOINIT_ATTR" $CAM; then
    ok "tras recuperar reinicia la placa para rearmar el ISP, con tope por encendido"
else
    mal "no reinicia tras recuperar (la imagen queda magenta) o lo hace sin tope (bucle de reinicios)"
fi
if grep -q "if (s_cam_corrupta)" $CAM; then
    ok "mientras la imagen esta corrupta NO se sirven fotos"
else
    mal "se sirven fotos de un sensor corrupto: ~350 KB de ruido a la galeria y a la app"
fi

AEMODE=$(python3 -c "import json;print(json.load(open('$CAMJSON'))['OV02C10']['agc'].get('mode',''))" 2>/dev/null)
if [ "$AEMODE" = "low_light_priority" ]; then
    ok "el AE protege las SOMBRAS (mode=$AEMODE): la cara a contraluz no queda negra"
else
    mal "el AE esta en modo '$AEMODE': con las luces altas prioritarias la cara a contraluz se queda en sombra (medido: Y=46 frente a 93)"
fi

echo
if [ "$fallos" -eq 0 ]; then echo "AUDITORIA OK"; exit 0; else echo "AUDITORIA: $fallos FALLOS"; exit 1; fi
