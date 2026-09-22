#!/usr/bin/env bash
#
# release.sh — prepara un release de "Joint SPL 145 Control".
#
# Uso:  ./release.sh [X.Y] ["mensaje del tag"]
#   ej: ./release.sh 1.12
#       ./release.sh 1.13 "Añade gráfico de consumo"
#       ./release.sh                      <- coge el último tag +0.1 (v2.5 -> v2.6)
#
# Qué hace (SÍ publica: sube código+tag y crea la Release en GitHub):
#   1. Comprueba que no hay cambios sin commitear.
#   2. Crea el tag anotado vX.Y sobre el commit actual.
#   3. Build LIMPIO forzando la regeneración de la versión/fecha (esquiva el
#      gotcha de ESP-IDF por el que el About mostraba una versión/fecha vieja).
#   4. Genera la imagen fusionada lista para el release.
#   5. Verifica que la versión embebida coincide con el tag.
#   6. Sube código y tag, y publica la Release en GitHub con el binario (esto
#      antes había que copiarlo y pegarlo a mano -- por eso la última Release
#      publicada llevaba tres semanas de retraso sobre el tag real, visto el
#      24-ago-2026, ver el comentario del paso 6 mas abajo).
#
# La versión que se ve en la pantalla (Ajustes → Acerca de) sale sola de este
# tag: no hay que editar ningún número en el código.
#
set -euo pipefail
cd "$(dirname "$0")"

REPO="Ehuntabi/victron-jc1060p470c-esp32p4"
# OJO: esto apuntaba a esp-idf-5.4 y por ahi se colaron las publicaciones de la
# v2.37 a la v2.43: el CI compilaba con 5.5.5 (correcto) pero los .bin que se
# publicaban los hace ESTE script, y salian con IDF 5.4.4 y el esp_video viejo
# (el arbol build/ arrastraba objetos de 5.4 y no se recompilaba el componente).
# Detectado el 22-sep-2026 al ver que el About de la placa decia 5.4.4.
# Ademas de corregir la ruta, mas abajo se COMPRUEBA la version dentro del .bin
# antes de publicar: si no coincide, aborta.
IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/esp-idf-5.5/export.sh}"
APP_BIN="build/joint_spl_145_control.bin"
# Directorio unico de releases (firmware P4 + app Flutter): solo la ULTIMA
# version de cada uno, para no liarse entre varios .bin/.apk sueltos por el
# home. Las versiones viejas se borran de aqui en el paso 4, no se acumulan.
RELDIR="$HOME/joint-releases"

# ── 0) argumentos ────────────────────────────────────────────────────────────
VER_IN="${1:-}"
if [ -z "$VER_IN" ]; then
  # Sin argumento: la siguiente version es el ultimo tag +0.1, que es la misma
  # regla que usa el build (nada de "-dirty"). Asi publicar la tanda de trabajo
  # que ya esta en el arbol no depende de acordarse del numero.
  ULTIMO_TAG="$(git describe --tags --abbrev=0 --match 'v*.*' 2>/dev/null || true)"
  if [ -z "$ULTIMO_TAG" ]; then
    echo "Uso: ./release.sh [X.Y] [\"mensaje del tag\"]   (ej: ./release.sh 2.6)"
    echo "     Sin X.Y se usa el ultimo tag +0.1, y ahora mismo no hay ningun tag v*."
    exit 1
  fi
  VER_IN="$(printf '%s' "$ULTIMO_TAG" | awk -F. '{ printf "%s.%d", $1, $2 + 1 }')"
  echo "[i] sin argumento: toca $VER_IN (ultimo tag $ULTIMO_TAG, +0.1)"
fi
VER="${VER_IN#v}"                                   # quita una 'v' inicial si la hay
# DOS numeros, no tres (24-ago-2026, decision del usuario). Con tres se acabo
# gastando un numero por commit: 81 etiquetas en seis semanas y 19 en un solo
# dia. Con dos, una version es una TANDA de trabajo, no un cambio suelto.
# Las etiquetas viejas de tres numeros siguen siendo validas y legibles.
if ! printf '%s' "$VER" | grep -Eq '^[0-9]+\.[0-9]+$'; then
  echo "ERROR: '$VER_IN' no tiene el formato X.Y (ej: 1.12)."
  echo "       Desde el 24-ago-2026 las versiones llevan DOS numeros, no tres."
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

# ── 2b) parches al IDF (idempotente) ────────────────────────────────────────
# El IDF que se usa para publicar lleva un arreglo OFICIAL de Espressif que aún
# no está en la version fijada. Sin el, un fallo de RAM DMA al hablar con la SD
# no devuelve error: revienta con "Load access fault" (memcpy desde NULL).
# Ver patches/LEEME.md. Si el IDF ya lo lleva (o el dia que se suba a 5.5.6+),
# esto no hace nada.
PARCHE_IDF="$PWD/patches/idf-5.5.5-spi-null-memcpy.patch"
if [ -f "$PARCHE_IDF" ] && [ -n "${IDF_PATH:-}" ]; then
  if grep -q 'buffer_to_rcv && trans_buf->buffer_to_rcv' \
       "$IDF_PATH/components/esp_driver_spi/src/gpspi/spi_master.c" 2>/dev/null; then
    echo "[ok] el IDF ya lleva el arreglo del memcpy con NULL (no hay que parchear)"
  elif git -C "$IDF_PATH" apply --check "$PARCHE_IDF" 2>/dev/null; then
    git -C "$IDF_PATH" apply "$PARCHE_IDF"
    echo "[ok] parche aplicado al IDF: $(basename "$PARCHE_IDF")"
  else
    echo "ERROR: el IDF no lleva el arreglo del memcpy con NULL y el parche no aplica."
    echo "       Revisa $PARCHE_IDF y el estado de $IDF_PATH (git -C \"\$IDF_PATH\" status)."
    exit 1
  fi
fi

# Anti-gotcha (ESP-IDF cachea la versión/fecha en el configure de CMake y no la
# refresca en builds incrementales -> el About mostraba datos viejos). Doble
# seguro: 'reconfigure' re-ejecuta CMake recapturando `git describe`, y borrar el
# .obj obliga a recompilar esp_app_desc con la fecha/versión de ahora.
# Build DESDE CERO. El incremental no basta: con el arbol de build/ viejo se
# mezclan objetos compilados con otra version de IDF y el binario sale diciendo
# una version que no es la que se esta usando (fue justo lo que paso).
# Unos minutos mas por publicacion, a cambio de que lo que se publica sea lo
# que se cree que es.
rm -rf build
idf.py build

# ── 3b) la version de ESP-IDF que lleva DENTRO el binario == la que se usa ─────
IDF_VER="$(idf.py --version 2>/dev/null | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
EN_BIN="$(strings "$APP_BIN" | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | sort -u | tr '\n' ' ')"
echo "[info] ESP-IDF en uso: $IDF_VER · versiones dentro del .bin: $EN_BIN"
case "$EN_BIN" in
  *"$IDF_VER"*) echo "[ok] el binario lleva el IDF con el que se ha compilado" ;;
  *) echo "ERROR: el .bin NO lleva $IDF_VER (lleva: $EN_BIN)."
     echo "       Casi seguro es un arbol de build mezclado: borra build/ y repite."
     exit 1 ;;
esac

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
  # Cortar AQUI: todavia no se ha subido ni publicado nada, asi que abortar es
  # gratis. Antes solo avisaba y seguia, y eso dejaba una Release publicada con
  # un .bin que dice otra version: justo el fichero que se lleva a la
  # autocaravana por OTA (auditoria del 13-sep-2026).
  echo "ERROR: la version embebida ('$EMB') no es la del tag ('$TAG'): NO publico nada." >&2
  echo "       Revisa 'git describe --tags' y si hay commits por encima del tag." >&2
  echo "       El tag $TAG se ha creado; borralo con:  git tag -d $TAG" >&2
  exit 1
fi

# ── 6) PUBLICAR: subir código, tag y Release ────────────────────────────────
#
# Esto lo hacía el humano copiando y pegando los comandos que el script
# imprimía... y por eso la última Release publicada era la v1.5.5 con 74 tags
# creados: el repo enseñaba como "Latest" una versión de hacía tres semanas
# mientras el aparato llevaba otra (visto el 24-ago-2026). Un paso que depende
# de que alguien se acuerde no es un paso del proceso, es una intención.
#
# Las notas salen del MENSAJE DEL TAG, que ya se escribe al crearlo. Si hay un
# NOTAS.md al lado, manda ese.
RAMA="$(git branch --show-current)"
echo "[..] subiendo código y tag"
git push origin "$RAMA" "$TAG"

if ! command -v gh >/dev/null 2>&1; then
  echo "AVISO: no está 'gh', así que la Release NO se ha publicado."
  echo "       Instálalo o publícala a mano:"
  echo "       gh release create $TAG -R $REPO --title \"Joint SPL 145 Control $TAG\" \\"
  echo "          --notes-file NOTAS.md \"$RELDIR/$OUT\""
elif gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  echo "[ok] la Release $TAG ya existe, no la toco"
else
  # Las notas SALEN DEL HISTORIAL, no de un fichero a mano.
  #
  # Aqui habia un "si existe NOTAS.md, usalo". NOTAS.md se escribio para la
  # v1.5.5 y nadie volvio a tocarlo, asi que las CUARENTA releases siguientes se
  # publicaron con el texto de la v1.5.5 (visto el 24-ago-2026). Un fichero que
  # hay que acordarse de actualizar acaba mintiendo; el log no.
  #
  # NOTAS.md sigue valiendo como texto a medida, pero SOLO si su primera linea
  # nombra esta version. Si habla de otra, se ignora y se avisa.
  NOTAS_TMP="/tmp/notas-$TAG.md"
  if [ -f NOTAS.md ] && head -1 NOTAS.md | grep -qF "$TAG"; then
    NOTAS=(--notes-file NOTAS.md)
  else
    if [ -f NOTAS.md ]; then
      echo "[i] NOTAS.md habla de otra version, lo ignoro y uso el historial"
    fi
    # "v*.*" vale para los dos formatos: el comodin abarca tambien el punto,
    # asi que encuentra igual v1.12 que las viejas v1.11.18.
    ANTERIOR=$(git describe --tags --abbrev=0 --match "v*.*" "$TAG^" 2>/dev/null || true)
    {
      if [ -n "$ANTERIOR" ]; then
        echo "Cambios desde $ANTERIOR:"
        echo
        git log --no-merges --format='- %s' "$ANTERIOR..$TAG"
      else
        echo "Primera version publicada."
      fi
      echo
      echo "---"
      echo "Firmware completo: SOLO para una placa virgen (borra los ajustes guardados)."
      echo "Para actualizar: \`idf.py flash\` por cable, o el app-bin por Wi-Fi en /ota."
    } > "$NOTAS_TMP"
    NOTAS=(--notes-file "$NOTAS_TMP")
  fi
  echo "[..] publicando la Release en GitHub"
  gh release create "$TAG" -R "$REPO" \
     --title "Joint SPL 145 Control $TAG" \
     "${NOTAS[@]}" \
     "$RELDIR/$OUT" "$RELDIR/joint-spl-145-control-$TAG-app.bin" \
    && echo "[ok] Release $TAG publicada con el firmware completo y el app-bin de OTA"
fi

# ── 7) lo que queda por hacer a mano ────────────────────────────────────────
cat <<EOF

────────────────────────────────────────────────────────────────────────────
PUBLICADO: código, tag y Release de $TAG. Solo queda grabar:

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
