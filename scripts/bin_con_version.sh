#!/usr/bin/env sh
#
# bin_con_version.sh — deja una copia del .bin con la VERSION en el nombre, para
# saber de un vistazo que imagen se lleva a la autocaravana.
#
# Lo llama CMakeLists.txt como paso POST_BUILD, asi que no hay que acordarse de
# ejecutarlo. A mano, por si acaso:
#     scripts/bin_con_version.sh build/joint_spl_145_control.bin
#
# La version es SIEMPRE un numero limpio "vX.Y": nunca "-dirty" ni "-N-g<hash>".
# Se saca del ultimo tag, y si el arbol tiene algo por encima (commits nuevos o
# cambios sin commitear) se le suma 0.1, que es la regla que sigue tambien
# CMakeLists.txt para la version que va dentro del binario:
#     joint_spl_145_control_v2.5.bin    arbol justo en el tag v2.5
#     joint_spl_145_control_v2.6.bin    v2.5 + trabajo sin etiquetar todavia
#
# Se calcula en CADA BUILD a proposito, no se reutiliza PROJECT_VER: esa se
# congela en el configure de CMake y un parche recien aplicado (que no toca
# .git/index) no lo dispara, asi que el .bin saldria con el numero de antes.
#
set -eu

BIN="${1:?uso: $0 <ruta del .bin>}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$BIN" ]; then
    echo "ERROR: no existe el binario '$BIN'" >&2
    exit 1
fi

# ── Version del arbol: tag exacto, o +0.1 si hay algo por encima ─────────────
VER_DESCRITA="$(git -C "$SRC_DIR" describe --always --tags --dirty --match 'v*.*' 2>/dev/null || true)"
V_ARBOL="$(printf '%s' "$VER_DESCRITA" | awk -F- '{
    if (NF == 1) { print $1 }
    else { split($1, p, "."); printf "%s.%d", p[1], p[2] + 1 }
}')"
if [ -z "$V_ARBOL" ]; then
    V_ARBOL="v?"
    echo "AVISO: 'git describe' no da version (¿arbol sin .git?); uso '$V_ARBOL'" >&2
fi

# ── Version EMBEBIDA en el binario (la que vera el aparato en Acerca de) ─────
# Mismo campo que lee release.sh para verificar el release: la version del
# app_desc, que esta en el byte 0x20+16 de la imagen.
V_BIN="$(python3 - "$BIN" <<'PY'
import sys
with open(sys.argv[1], 'rb') as f:
    d = f.read(0x120)
print(d[0x20 + 16:0x20 + 48].split(b'\x00')[0].decode('ascii', 'replace'))
PY
)"

if [ "$V_ARBOL" != "$V_BIN" ]; then
    echo "AVISO: el binario lleva embebida '$V_BIN' y el arbol ya va por '$V_ARBOL'." >&2
    echo "       El .bin se nombra con la del arbol, pero la pantalla (Acerca de)" >&2
    echo "       seguira diciendo '$V_BIN' hasta que rehagas la configuracion:" >&2
    echo "           idf.py reconfigure && idf.py build" >&2
fi

DIR="$(dirname "$BIN")"
OUT="$DIR/$(basename "$BIN" .bin)_${V_ARBOL}.bin"
cp "$BIN" "$OUT"
echo "[bin] $OUT"

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
