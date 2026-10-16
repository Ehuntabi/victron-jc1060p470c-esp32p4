#!/usr/bin/env bash
#
# grabar.sh — graba la P4 en la placa, pero SOLO si lo que se va a grabar esta
# publicado (commit, tag y Release en GitHub).
#
# ── POR QUE EXISTE (16-oct-2026, el usuario: "esto no puede pasar") ──────────
# La P4 del banco quedo grabada con la v4.38 -- el trabajo de ese dia -- mientras
# que en GitHub la ultima Release era la v4.37. El CODIGO si estaba subido (main
# iba al dia); lo que faltaba era la Release, que es lo que se descarga y lo que
# enseña el repo. Y la causa es de proceso: grabar es local e instantaneo,
# publicar depende de que alguien se acuerde de lanzar release.sh, asi que el
# aparato se adelanta al repo sin que nadie lo note hasta que se mira.
#
# La solucion no es acordarse mas: es que GRABAR exija estar publicado. Este
# script es el unico camino comodo para grabar, y comprueba antes:
#
#   1. que el arbol este limpio (nada sin commitear),
#   2. que HEAD este EXACTAMENTE en un tag v*.* (nada de adivinar versiones),
#   3. que ese tag este en 'origin' y apuntando al MISMO commit,
#   4. que haya 'gh' y que ese tag TENGA Release publicada, con el asset EXACTO
#      de esta version y su sha256 comprobado (16-oct-2026, ver mas abajo),
#   5. que en el puerto haya UN ESP32-P4 y no una de las cabinas (16-oct-2026),
# y solo entonces compila y graba. Si algo de eso falla, dice exactamente que
# falta y como arreglarlo (./release.sh X.Y "mensaje"), y NO graba nada.
#
# El paso 2 es de la auditoria del 15-oct-2026: antes la version se sacaba con
# `git describe` y, si habia commits por encima del ultimo tag, se le sumaba 0.1
# a mano. Asi se podia grabar trabajo en curso con el nombre de una version que
# NO existe: paso con esta P4, que decia v4.42 mientras la Release era la v4.41.
#
# ── EL ASSET TIENE QUE SER EL DE ESTA VERSION, NO "ALGUN .bin" (16-oct-2026) ─
# El paso 4 se conformaba con que la Release tuviera ALGUN fichero .bin: una
# Release con un asset de OTRA version (o con un .bin de otro proyecto subido a
# mano) pasaba, y la placa se grababa con algo que NO es lo que se descarga de
# esa version -- que es justo lo que este script promete. Ahora:
#   · el asset tiene que llamarse EXACTAMENTE joint-spl-145-control-<tag>-app.bin
#     (es el app-bin de OTA, que es el que corresponde al .bin que se compila
#     aqui; el "full" es otra cosa: la imagen fusionada para una placa virgen),
#   · se comprueba el sha256 que publica GitHub (digest) del fichero bajado,
#   · se comprueba que ESE fichero lleva dentro el tag (su app_desc),
#   · y se compara su sha256 con el del .bin compilado aqui:
#       - si coinciden, se graba el .bin local (es byte a byte el publicado),
#       - si no (la fecha de compilacion cambia en cada build: es lo normal si
#         release.sh compilo en otro momento), se graba EL PUBLICADO, para que
#         lo que quede en la placa sea exactamente lo descargable.
#
# ── EL PUERTO TIENE QUE SER EL DE ESTA PLACA (16-oct-2026) ──────────────────
# Esto grababa a ciegas. Ahora se comprueba la CLASE de puerto (la P4 sale como
# /dev/ttyACM* porque es USB-JTAG; la pantalla de 5" como /dev/ttyUSB* porque va
# por un CH340) y ADEMAS el CHIP que hay detras: esta placa es un ESP32-P4 y las
# dos cabinas son ESP32-S3, tambien en /dev/ttyACM*. Intercambiar los dos cables
# USB-JTAG (P4 <-> 3,5") hacia que este script grabara la 3,5" con el firmware
# de la P4: la clase de puerto no lo ve, el chip si.
# La comprobacion la hace el esptool del venv de ESP-IDF (v4) con `chip_id`, que
# solo LEE: no escribe nada.
#
# Para grabar a proposito algo sin publicar (una prueba en el banco) estan
# GRABAR_SIN_PUBLICAR=1 y el argumento --sin-publicar, que saltan las
# comprobaciones de publicacion (tag en 'origin' y Release) y AVISAN EN ROJO de
# que lo que queda en la placa no esta en GitHub. El paso 2 (estar en un tag) NO
# se salta: lo que se graba tiene que poder nombrarse con un tag de verdad, o no
# sabriamos ni que version es.
#
# Uso:  scripts/grabar.sh [puerto] [--rapido] [--sin-publicar] [--seco]
#         puerto         por defecto /dev/ttyACM0 (la P4; la 5" es ttyUSB0 y la
#                        3,5" ttyACM1) -- comprobar con: ls -l /dev/serial/by-id/
#         --rapido       sin compilar: graba el build/ que ya hay (y comprueba
#                        que ese .bin sea mas nuevo que TODO el fuente)
#         --sin-publicar igual que GRABAR_SIN_PUBLICAR=1
#         --seco         comprueba todo y NO escribe en la placa (para probar el
#                        propio script, o para ver que se grabaria)
#       GRABAR_PUERTO_RARO=1   salta la comprobacion de CLASE de puerto
#       GRABAR_CHIP_RARO=1     salta la comprobacion de CHIP (p4/s3)
#
# `pipefail` esta a proposito: sin el, un fallo de esptool se lo comia cualquier
# `| tail -N` de por medio y el script decia "GRABADO" tan tranquilo.
set -euo pipefail

AQUI="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$AQUI/.." && pwd)"
REPO_GH="Ehuntabi/victron-jc1060p470c-esp32p4"
IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/esp-idf-5.5/export.sh}"
APP_BIN="build/joint_spl_145_control.bin"
# El chip que tiene que haber en el puerto (paso 7) y los nombres EXACTOS de los
# assets publicados de esta version (paso 5; se completan con el tag mas abajo).
CHIP="esp32p4"
ASSET_FMT="joint-spl-145-control-%s-app.bin"
ASSET_FULL_FMT="joint-spl-145-control-%s-esp32p4-full.bin"
# La particion spiffs (la web del portal) se genera de esta carpeta.
SPIFFS_DIR="files"

# Colores para los avisos gordos: solo si la salida va a un terminal (si no, los
# codigos de escape acabarian dentro del fichero de log). Vale cualquiera de los
# dos canales: grabando desde la consola, stdout Y stderr son el terminal.
if [ -t 1 ] || [ -t 2 ]; then ROJO=$'\033[1;31m'; VERDE=$'\033[1;32m'; NORMAL=$'\033[0m'; else ROJO=""; VERDE=""; NORMAL=""; fi

# El puerto y los dos modos se leen de los argumentos, en cualquier orden (antes
# esto miraba solo $1 y $2, y con --sin-publicar en primer lugar se colaba como
# si fuera un puerto).
PUERTO="/dev/ttyACM0"
RAPIDO=0
SIN_PUBLICAR=0
SECO=0
VIA_SIN=""
[ "${GRABAR_SIN_PUBLICAR:-0}" = "1" ] && { SIN_PUBLICAR=1; VIA_SIN="GRABAR_SIN_PUBLICAR=1"; }
for a in "$@"; do
    case "$a" in
        --rapido)       RAPIDO=1 ;;
        --sin-publicar) SIN_PUBLICAR=1
                        VIA_SIN="${VIA_SIN:+$VIA_SIN y }--sin-publicar" ;;
        --seco)         SECO=1 ;;
        *)              PUERTO="$a" ;;
    esac
done

# Ficheros temporales (el asset publicado, la salida de esptool): se borran al
# salir pase lo que pase.
TMP=""
LOG_CHIP=""
trap 'rm -rf "${TMP:-}" "${LOG_CHIP:-}"' EXIT

# La version que lleva DENTRO un .bin de ESP-IDF: es el campo `version` del
# app_desc, en el offset 0x20+16 de la imagen. Es lo que ata un fichero al tag
# (el nombre del asset lo pone quien lo sube, esto lo pone el compilador).
version_de() {
    python3 - "$1" <<'PYBIN'
import sys
with open(sys.argv[1], 'rb') as f: d = f.read(0x120)
print(d[0x20+16:0x20+48].split(b'\x00')[0].decode('ascii', 'replace'))
PYBIN
}

# El by-id identifica la placa por su MAC, no por el orden en que se ha
# enchufado: si el log dice que se ha grabado en /dev/ttyACM1, esto permite
# saber DESPUES que aparato era. Es informativo: la comprobacion de verdad es el
# chip (paso 7).
by_id_de() {
    local objetivo byid
    objetivo="$(readlink -f "$1" 2>/dev/null || true)"
    [ -n "$objetivo" ] || return 0
    for byid in /dev/serial/by-id/*; do
        [ -e "$byid" ] || continue
        if [ "$(readlink -f "$byid" 2>/dev/null || true)" = "$objetivo" ]; then
            basename "$byid"
            return 0
        fi
    done
}

# ── EL PUERTO TIENE QUE SER EL DE ESTA PLACA: CLASE (16-oct-2026) ───────────
# Esto grababa a ciegas: con las placas conectadas, lanzar este script SIN
# argumentos apuntaba a un puerto que en este banco es la pantalla de 5 pulgadas
# (CH340). Resultado: el firmware de la P4 acababa en la pantalla de 5 pulgadas,
# y eso no se ve hasta que la pantalla no arranca. La P4 sale como /dev/ttyACM*
# (USB-JTAG) y la de 5 pulgadas como /dev/ttyUSB* (CH340).
#
# Si algun dia se graba con otro adaptador, se salta a proposito con
# GRABAR_PUERTO_RARO=1 (y queda dicho en el log).
case "$PUERTO" in
    /dev/ttyUSB*)
        if [ "${GRABAR_PUERTO_RARO:-0}" = "1" ]; then
            echo "${ROJO}AVISO:${NORMAL} grabando en $PUERTO, que NO es un puerto"
            echo "       de la P4 (USB-JTAG). Lo has pedido tu con GRABAR_PUERTO_RARO=1."
        else
            echo "NO GRABO: $PUERTO no es el puerto de la P4 (USB-JTAG)."
            echo "          En este banco, la P4 (USB-JTAG) es /dev/ttyACM0"
            echo "          y /dev/ttyUSB* es la pantalla de 5 pulgadas (CH340)."
            echo "          Grabar aqui metia el firmware en la placa que no es."
            echo
            echo "          Para saber cual es cual:   ls -l /dev/serial/by-id/"
            echo "          Si es a proposito (otro adaptador), repite con"
            echo "          GRABAR_PUERTO_RARO=1"
            exit 1
        fi
        ;;
esac

cd "$REPO"

# OJO, LO QUE **NO** HACE ESTE SCRIPT (dicho el 16-oct-2026): la P4 tiene DOS
# huecos de aplicacion (partitions.csv: ota_0 en 0x10000 y ota_1 en 0x410000) y
# aqui solo se graba `build/flash_args`, o sea la app en ota_0 y la ota_data. La
# ota_1 se queda con el firmware que tuviera. Hoy NO importa -- la P4 no se
# actualiza por OTA, se graba por cable -- pero si algun dia se usa OTA, la copia
# de respaldo estaria desfasada. La 5" si escribe sus dos mitades porque esa si
# usa OTA; la P4 no.
#
# ── 1) arbol limpio (dependencies.lock se reescribe solo en cada build) ──────
# sdkconfig se reescribe solo en cada build, asi que se restaura ANTES de mirar
# si el arbol esta limpio. OJO con hacerlo en UNA sola orden: en cabina5
# dependencies.lock NO esta versionado (esta en .gitignore) y `git checkout a b`
# aborta ENTERO si un pathspec no existe... con el `|| true` eso pasaba
# desapercibido y sdkconfig NO se restauraba, asi que cualquier churn del build
# dejaba el arbol "sucio" y el script se negaba a grabar/publicar (auditoria del
# 16-oct-2026). Dos ordenes separadas: cada una hace lo que puede.
git checkout -- sdkconfig 2>/dev/null || true
git checkout -- dependencies.lock 2>/dev/null || true
SUCIO="$(git status --porcelain | grep -v 'dependencies.lock' || true)"
if [ -n "$SUCIO" ]; then
    echo "NO GRABO: hay cambios sin commitear. Lo que se graba tiene que estar"
    echo "         en GitHub, y sin commitear no lo esta:"
    printf '%s\n' "$SUCIO" | sed 's/^/           /'
    echo
    echo "         Commitea (y sube) antes de grabar."
    exit 1
fi

# ── 2) la version de esta copia: HEAD tiene que estar EN un tag ─────────────
# ANTES esto sacaba la version con `git describe` y, si habia commits por encima
# del ultimo tag, le sumaba 0.1 a mano. Eso dejaba grabar trabajo en curso con el
# nombre de una version que NO existe (esta P4 llego a decir v4.42 con la Release
# publicada en la v4.41). Ahora no se adivina nada: o HEAD esta exactamente en un
# tag v*.*, o no se graba.
VER="$(git describe --tags --exact-match --match 'v*.*' 2>/dev/null || true)"
if [ -z "$VER" ]; then
    echo "NO GRABO: HEAD no esta exactamente en un tag v*.*, asi que esta copia no"
    echo "         es ninguna version publicada: tiene commits por encima del ultimo"
    echo "         tag (o no hay tags). Antes se le sumaba 0.1 a mano y se grababa"
    echo "         igual; asi se grabo la P4 diciendo v4.42 con la Release en v4.41."
    echo
    echo "         Publica esta tanda con ./release.sh (sin argumento coge el ultimo"
    echo "         tag +0.1) y vuelve a grabar."
    exit 1
fi
ASSET="$(printf "$ASSET_FMT" "$VER")"
ASSET_FULL="$(printf "$ASSET_FULL_FMT" "$VER")"
echo "[i] esta copia esta en el tag $VER   (HEAD $(git rev-parse --short HEAD))"

IMAGEN_APP="$APP_BIN"          # lo que se acaba grabando en el hueco de la app
SHA_PUB=""
SHA_LOCAL=""
if [ "$SIN_PUBLICAR" = "1" ]; then
    echo
    echo "${ROJO}════════════════════════════════════════════════════════════════${NORMAL}"
    echo "${ROJO}  AVISO: GRABO SIN COMPROBAR LA PUBLICACION ($VIA_SIN)${NORMAL}"
    echo "${ROJO}  Me salto el tag en 'origin' y la Release de GitHub: lo que${NORMAL}"
    echo "${ROJO}  quede en la placa NO estara publicado, y nadie podra${NORMAL}"
    echo "${ROJO}  descargarlo. Es una prueba de banco, no una version.${NORMAL}"
    echo "${ROJO}════════════════════════════════════════════════════════════════${NORMAL}"
    echo
else
    # ── 3) ese tag tiene que estar en 'origin' y en el MISMO commit ─────────
    # No se hace un `git fetch` obligatorio (tarda y no siempre hay red): se le
    # pregunta al remoto por ESE tag. El "^{}" desempaqueta el tag anotado, que es
    # lo que apunta al commit, y en local se compara lo mismo con "^{commit}". Un
    # tag que solo existe en este PC no lo puede descargar nadie: eso no es estar
    # publicado (que es lo que este script tiene que garantizar).
    REMOTO="$(git ls-remote origin "refs/tags/$VER^{}" 2>/dev/null | awk 'NR==1{print $1}' || true)"
    if [ -z "$REMOTO" ]; then
        # Sin "^{}": o el tag es LIGERO (los de estos repos los crea `git tag -a`,
        # o sea anotados, pero por si acaso) o el remoto no lo tiene. Se mira el
        # tag plano para poder distinguir las dos cosas.
        REMOTO="$(git ls-remote origin "refs/tags/$VER" 2>/dev/null | awk 'NR==1{print $1}' || true)"
    fi
    LOCAL="$(git rev-parse "refs/tags/$VER^{commit}" 2>/dev/null || true)"
    if [ -z "$REMOTO" ]; then
        echo
        echo "NO GRABO: el tag $VER no esta en 'origin' (o no hay red para preguntarlo)."
        echo "         Un tag que solo existe en este PC no lo puede descargar nadie,"
        echo "         asi que la placa se adelantaria al repo sin que se note."
        echo "         Subelo y vuelve:   git push origin $VER"
        exit 1
    fi
    if [ "$REMOTO" != "$LOCAL" ]; then
        echo
        echo "NO GRABO: el tag $VER del remoto apunta a OTRO commit que el de aqui."
        echo "         remoto: $REMOTO"
        echo "         local:  $LOCAL"
        echo "         Sube el tag que toca (git push origin $VER) o comprueba que esta"
        echo "         copia es la que se publico, antes de grabar."
        exit 1
    fi
    echo "[ok] $VER esta en origin y apunta al mismo commit ($(printf '%.7s' "$LOCAL"))"

    # ── 4) sin 'gh' no hay comprobacion posible: no se graba ────────────────
    # Antes, si no habia 'gh', el script AVISABA y seguia grabando: era la
    # puerta por la que se colaba una version sin Release publicada.
    if ! command -v gh >/dev/null 2>&1; then
        echo
        echo "NO GRABO: no hay 'gh' (GitHub CLI), asi que no puedo comprobar que la"
        echo "         $VER tenga Release publicada, y sin eso no se graba."
        echo "         Instala 'gh', autenticate (gh auth login) y vuelve a intentarlo."
        echo "         Para una prueba de banco:  --sin-publicar  (o GRABAR_SIN_PUBLICAR=1)"
        exit 1
    fi

    # ── 5) la Release, con EL asset de esta version y su sha256 ─────────────
    if ! gh release view "$VER" -R "$REPO_GH" >/dev/null 2>&1; then
        echo
        echo "NO GRABO: el tag $VER existe pero NO hay Release publicada en GitHub."
        echo "         (Pasa cuando release.sh creo el tag y luego fallo el push o no"
        echo "          estaba 'gh'. Si 'gh' no esta autenticado, esto tambien falla:"
        echo "          mira 'gh auth status'.)"
        echo
        echo "         Publicala:  ./release.sh ${VER#v} \"mensaje\""
        exit 1
    fi
    ASSETS="$(gh release view "$VER" -R "$REPO_GH" --json assets --jq '.assets[].name' 2>/dev/null || true)"
    # El nombre EXACTO: "algun .bin" no vale (auditoria del 16-oct-2026). El que
    # corresponde al .bin que se compila aqui es el app-bin de OTA; el "full" es
    # la imagen fusionada para una placa virgen y se comprueba aparte.
    if ! printf '%s\n' "$ASSETS" | grep -qxF "$ASSET"; then
        echo
        echo "NO GRABO: la Release $VER no tiene el asset '$ASSET'."
        echo "         Assets que tiene: ${ASSETS:-ninguno}"
        echo "         Una Release con otro .bin (o sin el app-bin) no es esta version:"
        echo "         lo que se bajaria de GitHub no es lo que se va a grabar."
        echo "         Vuelve a publicarla:  ./release.sh ${VER#v} \"mensaje\""
        exit 1
    fi
    if ! printf '%s\n' "$ASSETS" | grep -qxF "$ASSET_FULL"; then
        echo "${ROJO}AVISO:${NORMAL} la Release $VER no tiene '$ASSET_FULL' (la imagen"
        echo "       completa para una placa virgen). El app-bin si esta, asi que se"
        echo "       puede grabar; pero quien baje el .bin 'full' no lo encontrara."
    fi

    # El sha256 que publica GitHub (digest del asset) y el fichero de verdad.
    TMP="$(mktemp -d)"
    echo "[..] bajando el asset publicado para comprobarlo ($ASSET)"
    if ! gh release download "$VER" -R "$REPO_GH" --pattern "$ASSET" --dir "$TMP" >/dev/null 2>&1; then
        echo
        echo "NO GRABO: no he podido bajar '$ASSET' de la Release $VER."
        echo "         Sin el fichero no puedo comprobar que lo que se graba es lo"
        echo "         que se descarga. Mira 'gh auth status' y la red."
        exit 1
    fi
    SHA_PUB="$(sha256sum "$TMP/$ASSET" | awk '{print $1}')"
    DIG="$(gh release view "$VER" -R "$REPO_GH" --json assets \
           --jq ".assets[] | select(.name==\"$ASSET\") | .digest" 2>/dev/null || true)"
    case "$DIG" in
        sha256:*)
            if [ "${DIG#sha256:}" != "$SHA_PUB" ]; then
                echo
                echo "NO GRABO: el sha256 de '$ASSET' NO coincide con el que publica"
                echo "         GitHub (el fichero bajado esta corrupto o lo han"
                echo "         cambiado despues de subirlo)."
                echo "         GitHub: ${DIG#sha256:}"
                echo "         bajado: $SHA_PUB"
                exit 1
            fi
            echo "[ok] sha256 de $ASSET: $SHA_PUB (el que publica GitHub)" ;;
        ""|null)
            echo "${ROJO}AVISO:${NORMAL} GitHub no da sha256 de este asset (Release antiguo);"
            echo "       lo compruebo por dentro: sha256 del bajado = $SHA_PUB" ;;
        *)
            echo "${ROJO}AVISO:${NORMAL} GitHub da un digest que no entiendo ('$DIG');"
            echo "       sha256 del bajado = $SHA_PUB" ;;
    esac

    # Y ADEMAS el fichero bajado tiene que decir DENTRO que es esta version: es
    # lo unico que ata el asset al tag (el nombre lo pone quien sube).
    V_ASSET="$(version_de "$TMP/$ASSET")"
    if [ "$V_ASSET" != "$VER" ]; then
        echo
        echo "NO GRABO: el asset '$ASSET' lleva dentro '$V_ASSET' y esta version es"
        echo "         '$VER'. La Release de $VER tiene colgado un binario de otra"
        echo "         version: grabar eso seria mentir en el nombre."
        exit 1
    fi
    echo "[ok] el asset publicado dice '$V_ASSET' por dentro y su Release es $VER"
fi

# ── 6) el entorno de ESP-IDF, SIEMPRE (tambien con --rapido) ────────────────
# Hace falta por el esptool del venv (v4.12): el del sistema es la v5, que ya no
# acepta "--before default_reset" (usa guiones). Con --rapido esto no se cargaba
# y grabar fallaba en un terminal limpio (auditoria del 16-oct-2026).
if [ ! -f "$IDF_EXPORT" ]; then
    echo "ERROR: no encuentro ESP-IDF en $IDF_EXPORT"; exit 1
fi
# shellcheck disable=SC1090
# OJO con el "|| true": export.sh de ESP-IDF termina con un codigo
# distinto de cero y con "set -e" eso mataba este script en silencio
# (se quedo en la comprobacion y no compilaba nada, visto el 16-oct).
. "$IDF_EXPORT" >/dev/null 2>&1 || true
command -v idf.py >/dev/null 2>&1 || {
    echo "ERROR: ESP-IDF no ha cargado (idf.py no esta en el PATH)."
    exit 1
}
ESPTOOL_V="$(python3 -m esptool version 2>/dev/null | head -1 || true)"
case "$ESPTOOL_V" in
    *v4.*) : ;;
    *)  echo "ERROR: el 'python3 -m esptool' que hay en el PATH no es el del venv de"
        echo "       ESP-IDF (v4) sino '$ESPTOOL_V'. El del sistema (v5) ya no acepta"
        echo "       '--before default_reset' y la escritura fallaria."
        exit 1 ;;
esac

# ── 7) EL CHIP QUE HAY EN EL PUERTO (16-oct-2026) ───────────────────────────
# La P4 y la 3,5" son las dos /dev/ttyACM*, asi que la clase de puerto no las
# distingue: intercambiar los cables hacia que este script grabara la cabina con
# el firmware de la P4. `chip_id` solo LEE el chip (no escribe nada) y dice si
# es el que toca. La salida se guarda para que el log diga tambien la MAC de la
# placa que se ha grabado.
if [ "${GRABAR_CHIP_RARO:-0}" = "1" ]; then
    echo "${ROJO}AVISO:${NORMAL} me salto la comprobacion de chip (GRABAR_CHIP_RARO=1)"
else
    LOG_CHIP="$(mktemp)"
    echo "[..] comprobando que en $PUERTO hay un $CHIP (y no una cabina)"
    if ! python3 -m esptool --chip "$CHIP" -p "$PUERTO" -b 460800 \
            --before default_reset --after hard_reset chip_id >"$LOG_CHIP" 2>&1; then
        echo
        echo "NO GRABO: en $PUERTO no hay un $CHIP (o no se puede hablar con la placa)."
        echo "          Si el cable esta bien, pulsa BOOT, luego RESET, suelta RESET y"
        echo "          suelta BOOT, y repite. Lo que ha dicho esptool:"
        sed 's/^/            /' "$LOG_CHIP" | tail -4
        echo
        echo "          ¿Es otra placa a proposito? Entonces GRABAR_CHIP_RARO=1"
        exit 1
    fi
    echo "[ok] $(grep -m1 -i 'Chip is ' "$LOG_CHIP" | sed 's/^ *//') $(grep -m1 -o 'MAC: [0-9a-f:]*' "$LOG_CHIP" || true)"
fi
BYID="$(by_id_de "$PUERTO")"
[ -n "$BYID" ] && echo "[i] $PUERTO es $BYID"

# ── 8) compilar y grabar ───────────────────────────────────────────────────
if [ "$RAPIDO" = "0" ]; then
    echo "[..] compilando"
    idf.py build
fi
if [ ! -f "$APP_BIN" ]; then
    echo "ERROR: no hay $APP_BIN (compila primero, sin --rapido)"; exit 1
fi

# ── 8b) con --rapido, el .bin tiene que ser MAS NUEVO que el fuente ─────────
# Esta ruta se salta la compilacion a proposito (para grabar rapido lo que ya
# esta hecho), pero no puede grabar un build/ viejo: si un fichero de main/, de
# components/, el CMakeLists.txt, partitions.csv o el sdkconfig.defaults es mas
# nuevo que el .bin, ese binario NO lleva ese cambio y se grabaria firmware
# antiguo con la version nueva. (components/ y partitions.csv no se miraban:
# auditoria del 16-oct-2026.)
# OJO: sdkconfig NO se mira a proposito -- este script lo acaba de restaurar con
# `git checkout` y su fecha es de ahora mismo, asi que daria siempre positivo.
if [ "$RAPIDO" = "1" ]; then
    MAS_NUEVO="$(find main components CMakeLists.txt partitions.csv sdkconfig.defaults \
                 -type f -newer "$APP_BIN" -print -quit 2>/dev/null || true)"
    if [ -n "$MAS_NUEVO" ]; then
        echo "NO GRABO (--rapido): $APP_BIN es MAS ANTIGUO que $MAS_NUEVO, asi que"
        echo "                     ese .bin no lleva los ultimos cambios."
        echo "                     Compila (sin --rapido) y repite."
        exit 1
    fi
    # Y la web del portal: la particion spiffs se genera de files/, asi que un
    # spiffs.bin viejo se graba igual (va en flash_args) y la P4 seguiria
    # sirviendo la web de antes. Antes esto no se miraba.
    if [ -f build/spiffs.bin ] && [ -d "$SPIFFS_DIR" ]; then
        MAS_NUEVO_WEB="$(find "$SPIFFS_DIR" -type f -newer build/spiffs.bin -print -quit 2>/dev/null || true)"
        if [ -n "$MAS_NUEVO_WEB" ]; then
            echo "NO GRABO (--rapido): build/spiffs.bin (la web) es MAS ANTIGUO que"
            echo "                     $MAS_NUEVO_WEB: la P4 serviria la web de antes."
            echo "                     Compila (sin --rapido) y repite."
            exit 1
        fi
    fi
fi

V_BIN="$(version_de "$APP_BIN")"
echo "[i] el binario lleva dentro: $V_BIN   (esta copia es $VER)"
if [ "$V_BIN" != "$VER" ]; then
    echo "ERROR: el .bin dice '$V_BIN' y esta copia es '$VER'."
    echo "       Casi seguro es un build/ viejo: borra build/ y repite (asi lo hace"
    echo "       release.sh, y por eso lo que se publica es lo que se cree que es)."
    exit 1
fi

# ── 9) ¿el .bin local es el publicado? ─────────────────────────────────────
# Si lo es, se graba el local (son el mismo fichero). Si no, se graba EL
# PUBLICADO: la diferencia normal es la fecha de compilacion (cada build mete la
# suya en el app_desc), pero lo que tiene que quedar en la placa es exactamente
# lo que se descarga, que es lo que este script promete.
ARGS_FLASH=""                  # vacio = usar build/flash_args tal cual
if [ "$SIN_PUBLICAR" = "0" ]; then
    SHA_LOCAL="$(sha256sum "$APP_BIN" | awk '{print $1}')"
    if [ "$SHA_LOCAL" = "$SHA_PUB" ]; then
        echo "[ok] el .bin de aqui es byte a byte el publicado (sha256 $SHA_LOCAL)"
    else
        IMAGEN_APP="$TMP/$ASSET"
        ARGS_FLASH="$TMP/flash_args"
        # Se reescribe la lista de offsets del build cambiando SOLO la app: el
        # bootloader, la tabla de particiones, la ota_data y la spiffs salen del
        # build local, que es del mismo tag (GitHub no publica esos ficheros).
        awk -v viejo="$(basename "$APP_BIN")" -v nuevo="$IMAGEN_APP" \
            '$1 ~ /^0x/ && $2 == viejo { print $1, nuevo; next } { print }' \
            build/flash_args > "$ARGS_FLASH"
        if ! grep -qF "$IMAGEN_APP" "$ARGS_FLASH" || \
           grep -qE "(^|[[:space:]])$(basename "$APP_BIN")\$" "$ARGS_FLASH"; then
            echo "ERROR: no he podido cambiar la app en build/flash_args; NO grabo."
            exit 1
        fi
        echo "${ROJO}OJO:${NORMAL} el .bin de aqui NO es el publicado (cambia la fecha de"
        echo "     compilacion: sha256 local $SHA_LOCAL / publicado $SHA_PUB)."
        echo "     Se graba EL PUBLICADO, para que la placa lleve justo lo descargable."
    fi
fi

if [ "$SECO" = "1" ]; then
    echo
    echo "${VERDE}SECO:${NORMAL} no escribo nada en $PUERTO. Habria grabado:"
    echo "      version  $VER"
    echo "      imagen   $IMAGEN_APP"
    [ "$SIN_PUBLICAR" = "0" ] && echo "      sha256   $SHA_PUB (el de la Release)"
    echo "      offsets  ${ARGS_FLASH:-build/flash_args}"
    exit 0
fi

# SE GRABA CON esptool Y NO CON `idf.py flash`, y es a proposito: `idf.py flash`
# compila OTRA VEZ antes de grabar, y ese segundo build puede recalcular la
# version (el propio build reescribe dependencies.lock y sdkconfig) y grabar algo
# DISTINTO de lo que se acaba de verificar. Aqui la imagen ya esta hecha y
# comprobada: se escribe y punto. `build/flash_args` es la lista de offsets que
# mantiene el propio build (bootloader, tabla de particiones, app, spiffs).
echo "[..] grabando en $PUERTO"
( cd build && python3 -m esptool --chip esp32p4 -p "$PUERTO" -b 460800 \
    --before default_reset --after hard_reset write_flash "@${ARGS_FLASH:-flash_args}" )
echo
echo "GRABADO $V_BIN en $PUERTO."
if [ "$SIN_PUBLICAR" = "1" ]; then
    echo "${ROJO}OJO: es una copia SIN PUBLICAR ($VIA_SIN). GitHub no la tiene.${NORMAL}"
else
    echo "Y publicado: el tag $V_BIN esta en origin, con su Release y su $ASSET"
    echo "(sha256 $SHA_PUB${SHA_LOCAL:+; el .bin de aqui, $SHA_LOCAL})."
fi
