#!/usr/bin/env sh
#
# bin_con_version.sh — deja una copia del .bin con la VERSION en el nombre, para
# saber de un vistazo que imagen se lleva a la autocaravana.
#
# Lo llama CMakeLists.txt (target bin_con_version), asi que no hay que acordarse
# de ejecutarlo. A mano, por si acaso:
#     scripts/bin_con_version.sh build/joint_spl_145_control.bin
#
# La version es SIEMPRE un numero limpio "vX.Y" (o "vX.Y.Z" para los tags viejos
# de tres numeros): nunca "-dirty" ni "-N-g<hash>". Se saca del ultimo tag, y si
# el arbol tiene algo por encima (commits nuevos o cambios sin commitear) se le
# suma 0.1 al segundo numero, que es la regla que sigue tambien CMakeLists.txt
# para la version que va DENTRO del binario:
#     joint_spl_145_control_v2.5.bin    arbol justo en el tag v2.5
#     joint_spl_145_control_v2.6.bin    v2.5 + trabajo sin etiquetar todavia
#
# ── DOS COSAS QUE NO SE PUEDEN HACER (auditoria del 13-sep-2026) ─────────────
# 1. NO fiarse de la version del arbol para nombrar el fichero que se lleva: un
#    parche aplicado sin commitear no dispara el reconfigure de CMake, asi que el
#    .bin puede seguir llevando la version VIEJA dentro mientras el arbol ya va
#    por la siguiente. Se detectaba, se avisaba... y se copiaba igual: el fichero
#    de ~/joint-releases quedaba con un nombre que no era lo que llevaba dentro, y
#    encima borraba el bueno. Ahora, si no coinciden, NO SE TOCA ~/joint-releases
#    (ni copiar ni borrar) y la copia del build se nombra con lo que lleva DENTRO.
# 2. NO tumbar la compilacion: esto es un extra. Si falta python3, si git no da
#    version o si no se puede copiar, se avisa y se sigue (exit 0).
#
set -u

BIN="${1:?uso: $0 <ruta del .bin>}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$BIN" ]; then
    echo "AVISO: no existe el binario '$BIN'; no hay copia con version" >&2
    exit 0
fi

# ── Version del arbol: el tag, o +0.1 si hay algo por encima ─────────────────
# Sin --always (un hash no es una version) y sin --dirty (la suciedad se mira
# aparte, para poder ignorar dependencies.lock, que se reescribe solo en cada
# build y no es un cambio del programa).
VER_DESCRITA="$(git -C "$SRC_DIR" describe --tags --match 'v*.*' 2>/dev/null || true)"
SUCIO="$(git -C "$SRC_DIR" status --porcelain --untracked-files=no -- . \
         ':(exclude)dependencies.lock' 2>/dev/null || true)"
AHEAD="$(printf '%s' "$VER_DESCRITA" | awk -F- '{ print (NF > 1) ? 1 : 0 }')"
[ -n "$SUCIO" ] && AHEAD=1

V_ARBOL="$(printf '%s' "$VER_DESCRITA" | awk -F- -v ahead="$AHEAD" '
    NF == 0 { exit }
    {
        tag = $1
        if (tag !~ /^v[0-9]+\.[0-9]+(\.[0-9]+)?$/) exit     # no hay version usable
        if (ahead == 1) { split(tag, p, "."); printf "%s.%d", p[1], p[2] + 1 }
        else            { print tag }
    }')"

# ── Version EMBEBIDA en el binario (la que vera el aparato en Acerca de) ─────
# Mismo campo que lee release.sh para verificar el release: la version del
# app_desc, que esta en el byte 0x20+16 de la imagen. Best-effort: sin python3
# esto no puede fallar, solo deja de comprobarse.
V_BIN=""
if command -v python3 >/dev/null 2>&1; then
    V_BIN="$(python3 - "$BIN" 2>/dev/null <<'PY'
import sys
try:
    with open(sys.argv[1], 'rb') as f:
        d = f.read(0x120)
    print(d[0x20 + 16:0x20 + 48].split(b'\x00')[0].decode('ascii', 'replace'))
except Exception:
    pass
PY
)"
fi
es_version() { printf '%s' "$1" | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$'; }

if ! es_version "$V_ARBOL"; then
    echo "AVISO: el arbol no da una version utilizable ('$VER_DESCRITA')." >&2
    echo "       No dejo copia con version (el .bin de siempre esta donde toca)." >&2
    exit 0
fi

# ── Copia junto al binario, en build/ ────────────────────────────────────────
# Se nombra con lo que LLEVA DENTRO (V_BIN) cuando se sabe; si no se ha podido
# leer, con la del arbol. Asi el nombre nunca miente sobre el contenido.
V_NOMBRE="$V_ARBOL"
DESAJUSTE=0
if [ -n "$V_BIN" ] && [ "$V_BIN" != "$V_ARBOL" ]; then
    DESAJUSTE=1
    V_NOMBRE="$V_BIN"
fi

DIR="$(dirname "$BIN")"
OUT="$DIR/$(basename "$BIN" .bin)_${V_NOMBRE}.bin"
if cp "$BIN" "$OUT" 2>/dev/null; then
    echo "[bin] $OUT"
else
    echo "AVISO: no he podido dejar la copia con version en $DIR" >&2
    exit 0
fi

if [ "$DESAJUSTE" = "1" ]; then
    echo "AVISO: el binario lleva embebida '$V_BIN' y el arbol ya va por '$V_ARBOL'." >&2
    echo "       No toco la carpeta de releases: ese .bin no es lo que dice su version." >&2
    echo "       Rehaz la configuracion y compila otra vez:" >&2
    echo "           idf.py reconfigure && idf.py build" >&2
    exit 0
fi

# ── Solo el repo de publicacion toca la carpeta de releases ──────────────────
# Los arboles de banco (.scratch/pantallas y compania) comparten este script, pero
# NO son el repo: el 22-sep-2026 un build de banco copio su .bin (v2.43) a
# ~/joint-releases y borro de ahi los de la v3.6 ya publicada. La carpeta de
# releases es solo para lo que sale del repo real (JOINT_REPO_DIR lo cambia).
REPO_REAL="${JOINT_REPO_DIR:-$HOME/joint/victron}"
if [ "$(cd "$SRC_DIR" && pwd -P)" != "$(cd "$REPO_REAL" 2>/dev/null && pwd -P)" ]; then
    echo "AVISO: '$SRC_DIR' no es el repo de publicacion ($REPO_REAL)." >&2
    echo "       No toco la carpeta de releases; la copia del build esta en $OUT" >&2
    exit 0
fi

# ── Copia a la carpeta de releases (la que se lleva a la autocaravana) ───────
# Mismo directorio y MISMO nombre que publica release.sh para el binario de OTA
# (joint-spl-145-control-vX.Y-app.bin), para que lo que se lleva a la
# autocaravana este siempre en un sitio conocido. Se deja SOLO esta version:
# las versiones viejas del firmware del P4 se retiran igual que hace release.sh
# al publicar (el .bin viejo sigue estando en la Release de GitHub y en su tag).
# No se toca nada de otros proyectos (35cabina) ni la app Flutter.
# Destino alternativo para pruebas: JOINT_RELEASES_DIR.
RELDIR="${JOINT_RELEASES_DIR:-$HOME/joint-releases}"
RELBIN="$RELDIR/$(basename "$BIN" .bin | tr '_' '-')-${V_ARBOL}-app.bin"
if mkdir -p "$RELDIR" 2>/dev/null && cp "$BIN" "$RELBIN" 2>/dev/null; then
    echo "[bin] $RELBIN"
    for viejo in "$RELDIR"/joint-spl-145-control-v*-app.bin \
                 "$RELDIR"/joint-spl-145-control-v*-esp32p4-full.bin; do
        [ -e "$viejo" ] || continue
        case "$viejo" in
            *"-${V_ARBOL}-"*) continue ;;   # la de este build: se queda
        esac
        rm -f "$viejo" && echo "[bin] fuera la version vieja: $(basename "$viejo")"
    done
else
    echo "AVISO: no he podido copiar a $RELDIR (¿permisos?). La copia del build esta en $OUT" >&2
fi
exit 0
