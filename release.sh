#!/usr/bin/env bash
#
# release.sh — prepara un release de "Joint SPL 145 Control".
#
# Uso:  ./release.sh X.Y.Z  ["mensaje del tag"]
#   ej: ./release.sh 1.0.1
#       ./release.sh 1.1.0 "Añade gráfico de consumo"
#
# Qué hace (TODO en local, no publica nada):
#   1. Comprueba que no hay cambios sin commitear.
#   2. Crea el tag anotado vX.Y.Z sobre el commit actual.
#   3. Build LIMPIO forzando la regeneración de la versión/fecha (esquiva el
#      gotcha de ESP-IDF por el que el About mostraba una versión/fecha vieja).
#   4. Genera la imagen fusionada lista para el release.
#   5. Verifica que la versión embebida coincide con el tag.
#   6. Te imprime los comandos de PUSH, de crear la Release y de flashear.
#
# La versión que se ve en la pantalla (Ajustes → Acerca de) sale sola de este
# tag: no hay que editar ningún número en el código.
#
set -euo pipefail
cd "$(dirname "$0")"

REPO="Ehuntabi/victron-jc1060p470c-esp32p4"
IDF_EXPORT="$HOME/.espressif/esp-idf-5.4/export.sh"
APP_BIN="build/joint_spl_145_control.bin"
# Directorio unico de releases (firmware P4 + app Flutter): solo la ULTIMA
# version de cada uno, para no liarse entre varios .bin/.apk sueltos por el
# home. Las versiones viejas se borran de aqui en el paso 4, no se acumulan.
RELDIR="$HOME/joint-releases"

# ── 0) argumentos ────────────────────────────────────────────────────────────
VER_IN="${1:-}"
if [ -z "$VER_IN" ]; then
  echo "Uso: ./release.sh X.Y.Z [\"mensaje del tag\"]   (ej: ./release.sh 1.0.1)"
  exit 1
fi
VER="${VER_IN#v}"                                   # quita una 'v' inicial si la hay
if ! printf '%s' "$VER" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "ERROR: '$VER_IN' no tiene el formato X.Y.Z (ej: 1.0.1)."
  exit 1
fi
TAG="v$VER"
MSG="${2:-Joint SPL 145 Control $TAG}"

# ── 1) working tree limpio (ignorando el churn conocido de dependencies.lock) ─
git checkout dependencies.lock 2>/dev/null || true
if [ -n "$(git status --porcelain | grep -v 'dependencies.lock' || true)" ]; then
  echo "ERROR: tienes cambios sin commitear. Haz commit antes de releasear:"
  git status --short
  exit 1
fi

# ── 2) crear el tag (aborta si ya existe) ────────────────────────────────────
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "ERROR: el tag $TAG ya existe. Usa otro número o bórralo con: git tag -d $TAG"
  exit 1
fi
git tag -a "$TAG" -m "$MSG"
echo "[ok] tag $TAG creado sobre $(git rev-parse --short HEAD)"

# ── 3) build limpio (fuerza recompilar la versión/fecha) ─────────────────────
if [ ! -f "$IDF_EXPORT" ]; then
  echo "ERROR: no encuentro ESP-IDF en $IDF_EXPORT"; exit 1
fi
# shellcheck disable=SC1090
. "$IDF_EXPORT" >/dev/null 2>&1
# Anti-gotcha (ESP-IDF cachea la versión/fecha en el configure de CMake y no la
# refresca en builds incrementales -> el About mostraba datos viejos). Doble
# seguro: 'reconfigure' re-ejecuta CMake recapturando `git describe`, y borrar el
# .obj obliga a recompilar esp_app_desc con la fecha/versión de ahora.
idf.py reconfigure >/dev/null 2>&1 || true
find build -name esp_app_desc.c.obj -delete 2>/dev/null || true
idf.py build

# ── 4) imagen fusionada para el release ──────────────────────────────────────
# Limpiar ANTES de generar la nueva: el patron de un release anterior en
# build/ coincide con el nombre que se esta a punto de crear (mismo prefijo
# "-esp32p4-full.bin"), asi que borrar despues se cargaba la recien creada.
rm -f build/joint-spl-145-control-v*-esp32p4-full.bin

OUT="joint-spl-145-control-$TAG-esp32p4-full.bin"
( cd build && python -m esptool --chip esp32p4 merge_bin -o "$OUT" @flash_args )

# Mover a RELDIR como LA UNICA version presente: borrar antes cualquier .bin
# de firmware que hubiera de un release anterior (full o app-only).
mkdir -p "$RELDIR"
rm -f "$RELDIR"/joint-spl-145-control-v*-esp32p4-full.bin \
      "$RELDIR"/joint-spl-145-control-v*-app.bin
mv "build/$OUT" "$RELDIR/"
cp "$APP_BIN" "$RELDIR/joint-spl-145-control-$TAG-app.bin"
echo "[ok] copiado a $RELDIR (full + app), version anterior borrada de ahi y de build/"

# ── 5) verificar que la versión embebida == tag ──────────────────────────────
EMB="$(python3 - "$APP_BIN" <<'PY'
import sys
with open(sys.argv[1],'rb') as f: d=f.read(0x120)
print(d[0x20+16:0x20+48].split(b'\x00')[0].decode('ascii','replace'))
PY
)"
echo "[info] versión embebida en el binario: '$EMB'   (tag: '$TAG')"
if [ "$EMB" != "$TAG" ]; then
  echo "AVISO: no coinciden. Revisa 'git describe --tags' (¿hay commits por encima del tag?)."
fi

# ── 6) siguientes pasos (a mano, cuando quieras publicar) ────────────────────
cat <<EOF

────────────────────────────────────────────────────────────────────────────
LISTO EN LOCAL. Para publicar, ejecuta cuando quieras:

  # 1) subir código + tag
  git push origin main $(git branch --show-current) $TAG

  # 2) crear la Release en GitHub (escribe antes las notas en un fichero .md)
  gh release create $TAG -R $REPO \\
     --title "Joint SPL 145 Control $TAG" \\
     --notes-file NOTAS.md \\
     "$RELDIR/$OUT"

  # 3) grabar en la placa
  idf.py -p /dev/ttyACM0 flash

  # OTA por el portal Wi-Fi: sube $RELDIR/joint-spl-145-control-$TAG-app.bin
────────────────────────────────────────────────────────────────────────────

Unico .bin de cada tipo en $RELDIR (firmware full + app-bin para OTA).

⚠️  EL .bin "full" ES PARA UNA PLACA VIRGEN, NO PARA ACTUALIZAR.
    merge_bin rellena con 0xFF los huecos entre particiones, y en uno de esos
    huecos esta la NVS: grabarlo desde el principio BORRA TODOS LOS AJUSTES
    guardados (umbrales del frigo, red Wi-Fi, claves Victron, calibraciones...)
    y todo vuelve a los valores de fabrica del programa.
    Para actualizar: "idf.py -p PUERTO flash" o el app-bin por OTA.
EOF
