#!/usr/bin/env bash
#
# grabar.sh — graba la P4 en la placa, pero SOLO si lo que se va a grabar esta
# publicado (commit, tag y Release en GitHub).
#
# ── POR QUE EXISTE (16-oct-2026, el usuario: "esto no puede pasar") ──────────
# La P4 del banco quedo grabada con la v4.38 -- el trabajo de ese dia, con los
# contadores del viaje, el km del cuadro y la lectura al cerrar -- mientras que
# en GitHub la ultima Release era la v4.37. El codigo SI estaba subido (main iba
# al dia); lo que faltaba era la Release, que es lo que se descarga y lo que
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
#   4. que haya 'gh' y que ese tag TENGA Release publicada en GitHub, con .bin,
# y solo entonces compila y graba. Si algo de eso falla, dice exactamente que
# falta y como arreglarlo (./release.sh X.Y "mensaje"), y NO graba nada.
#
# El paso 2 es de la auditoria del 15-oct-2026: antes la version se sacaba con
# `git describe` y, si habia commits por encima del ultimo tag, se le sumaba 0.1
# a mano. Asi se podia grabar trabajo en curso con el nombre de una version que
# NO existe: paso con esta P4, que decia v4.42 mientras la Release era la v4.41.
#
# Para grabar a proposito algo sin publicar (una prueba en el banco) estan
# GRABAR_SIN_PUBLICAR=1 y el argumento --sin-publicar, que saltan las
# comprobaciones de publicacion (tag en 'origin' y Release) y AVISAN EN ROJO de
# que lo que queda en la placa no esta en GitHub. El paso 2 (estar en un tag) NO
# se salta: lo que se graba tiene que poder nombrarse con un tag de verdad, o no
# sabriamos ni que version es.
#
# Uso:  scripts/grabar.sh [puerto] [--rapido] [--sin-publicar]
#         puerto         por defecto /dev/ttyACM0 (la P4; la 5" es ttyUSB0)
#         --rapido       sin compilar: graba el build/ que ya hay (y comprueba
#                        que ese .bin sea mas nuevo que el fuente de main/)
#         --sin-publicar igual que GRABAR_SIN_PUBLICAR=1
#
# `pipefail` esta a proposito: sin el, un fallo de esptool se lo comia cualquier
# `| tail -N` de por medio y el script decia "GRABADO" tan tranquilo.
set -euo pipefail

AQUI="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$AQUI/.." && pwd)"
REPO_GH="Ehuntabi/victron-jc1060p470c-esp32p4"
IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/esp-idf-5.5/export.sh}"
APP_BIN="build/joint_spl_145_control.bin"

# Colores para los avisos gordos: solo si la salida va a un terminal (si no, los
# codigos de escape acabarian dentro del fichero de log). Vale cualquiera de los
# dos canales: grabando desde la consola, stdout Y stderr son el terminal.
if [ -t 1 ] || [ -t 2 ]; then ROJO=$'\033[1;31m'; NORMAL=$'\033[0m'; else ROJO=""; NORMAL=""; fi

# El puerto y los dos modos se leen de los argumentos, en cualquier orden (antes
# esto miraba solo $1 y $2, y con --sin-publicar en primer lugar se colaba como
# si fuera un puerto).
PUERTO="/dev/ttyACM0"
RAPIDO=0
SIN_PUBLICAR=0
VIA_SIN=""
[ "${GRABAR_SIN_PUBLICAR:-0}" = "1" ] && { SIN_PUBLICAR=1; VIA_SIN="GRABAR_SIN_PUBLICAR=1"; }
for a in "$@"; do
    case "$a" in
        --rapido)       RAPIDO=1 ;;
        --sin-publicar) SIN_PUBLICAR=1
                        VIA_SIN="${VIA_SIN:+$VIA_SIN y }--sin-publicar" ;;
        *)              PUERTO="$a" ;;
    esac
done

# ── EL PUERTO TIENE QUE SER EL DE ESTA PLACA (16-oct-2026) ──────────────────
#
# Esto grababa a ciegas: con las placas conectadas, lanzar este script SIN
# argumentos apuntaba a un puerto que en este banco es la pantalla de 5 pulgadas (CH340). Resultado: el
# firmware de la P4 acababa en la pantalla de 5 pulgadas (CH340), y eso no se ve hasta que
# la pantalla no arranca. Aqui se comprueba la CLASE de puerto: la P4 sale
# como /dev/ttyACM* y la pantalla de 5 pulgadas (CH340) como /dev/ttyUSB*.
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
git checkout dependencies.lock 2>/dev/null || true
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
echo "[i] esta copia esta en el tag $VER   (HEAD $(git rev-parse --short HEAD))"

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
    # ── 5) ¿tiene Release publicada, y con al menos un .bin? ────────────────
    if ! gh release view "$VER" -R "$REPO_GH" >/dev/null 2>&1; then
        echo
        echo "NO GRABO: el tag $VER existe pero NO hay Release publicada en GitHub."
        echo "         (Pasa cuando release.sh creo el tag y luego fallo el push o no"
        echo "          estaba 'gh'. Si 'gh' no esta autenticado, esto tambien falla:"
        echo "          mira 'gh auth status'.)"
        echo
        echo "         Publicala:  gh release create $VER -R $REPO_GH \\"
        echo "                       --title \"Joint SPL 145 Control $VER\" \\"
        echo "                       --notes-from-tag \"\$HOME/joint-releases/joint-spl-145-control-$VER-esp32p4-full.bin\""
        exit 1
    fi
    BINS="$(gh release view "$VER" -R "$REPO_GH" --json assets --jq '.assets[].name' 2>/dev/null || true)"
    if ! printf '%s\n' "$BINS" | grep -q '\.bin$'; then
        echo
        echo "NO GRABO: la Release $VER existe pero NO tiene ningun asset .bin"
        echo "         (visto: ${BINS:-ninguno}). Una Release sin firmware no es una"
        echo "         version publicada: no hay nada que descargar."
        echo "         Sube el .bin:  gh release upload $VER -R $REPO_GH <fichero.bin>"
        exit 1
    fi
    echo "[ok] $VER tiene tag en origin, Release publicada y .bin ($(printf '%s' "$BINS" | tr '\n' ' '))"
fi

# ── 6) compilar y grabar ───────────────────────────────────────────────────
if [ "$RAPIDO" = "0" ]; then
    if [ ! -f "$IDF_EXPORT" ]; then
        echo "ERROR: no encuentro ESP-IDF en $IDF_EXPORT"; exit 1
    fi
    # shellcheck disable=SC1090
    # shellcheck disable=SC1090
    # OJO con el "|| true": export.sh de ESP-IDF termina con un codigo
    # distinto de cero y con "set -e" eso mataba este script en silencio
    # (se quedo en la comprobacion y no compilaba nada, visto el 16-oct).
    . "$IDF_EXPORT" >/dev/null 2>&1 || true
    command -v idf.py >/dev/null 2>&1 || {
        echo "ERROR: ESP-IDF no ha cargado (idf.py no esta en el PATH)."
        exit 1
    }
    echo "[..] compilando"
    idf.py build
fi
if [ ! -f "$APP_BIN" ]; then
    echo "ERROR: no hay $APP_BIN (compila primero, sin --rapido)"; exit 1
fi

# ── 6b) con --rapido, el .bin tiene que ser MAS NUEVO que el fuente ─────────
# Esta ruta se salta la compilacion a proposito (para grabar rapido lo que ya
# esta hecho), pero no puede grabar un build/ viejo: si un fichero de main/ (o el
# CMakeLists.txt o el sdkconfig.defaults) es mas nuevo que el .bin, ese binario
# NO lleva ese cambio y se grabaria firmware antiguo con la version nueva.
if [ "$RAPIDO" = "1" ]; then
    MAS_NUEVO="$(find main CMakeLists.txt sdkconfig.defaults -type f -newer "$APP_BIN" -print -quit 2>/dev/null || true)"
    if [ -n "$MAS_NUEVO" ]; then
        echo "NO GRABO (--rapido): $APP_BIN es MAS ANTIGUO que $MAS_NUEVO, asi que"
        echo "                     ese .bin no lleva los ultimos cambios."
        echo "                     Compila (sin --rapido) y repite."
        exit 1
    fi
fi

V_BIN="$(python3 - "$APP_BIN" <<'PY'
import sys
with open(sys.argv[1],'rb') as f: d=f.read(0x120)
print(d[0x20+16:0x20+48].split(b'\x00')[0].decode('ascii','replace'))
PY
)"
echo "[i] el binario lleva dentro: $V_BIN   (esta copia es $VER)"
if [ "$V_BIN" != "$VER" ]; then
    echo "ERROR: el .bin dice '$V_BIN' y esta copia es '$VER'."
    echo "       Casi seguro es un build/ viejo: borra build/ y repite (asi lo hace"
    echo "       release.sh, y por eso lo que se publica es lo que se cree que es)."
    exit 1
fi

# SE GRABA CON esptool Y NO CON `idf.py flash`, y es a proposito: `idf.py flash`
# compila OTRA VEZ antes de grabar, y ese segundo build puede recalcular la
# version (el propio build reescribe dependencies.lock y sdkconfig) y grabar algo
# DISTINTO de lo que se acaba de verificar. Aqui la imagen ya esta hecha y
# comprobada: se escribe y punto. `build/flash_args` es la lista de offsets que
# mantiene el propio build (bootloader, tabla de particiones, app, spiffs).
echo "[..] grabando en $PUERTO"
( cd build && python3 -m esptool --chip esp32p4 -p "$PUERTO" -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args )
echo
echo "GRABADO $V_BIN en $PUERTO."
if [ "$SIN_PUBLICAR" = "1" ]; then
    echo "${ROJO}OJO: es una copia SIN PUBLICAR ($VIA_SIN). GitHub no la tiene.${NORMAL}"
else
    echo "Y publicado: el tag $V_BIN esta en origin, con su Release y su .bin."
fi
