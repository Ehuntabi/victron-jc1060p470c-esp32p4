#!/usr/bin/env bash
# Trae las capturas de la pantalla al repo, listas para commitear.
#
# La pantalla las guarda en /sdcard/screenshots con nombre ya ordenado
# (00_overview.jpg, 01_bateria.jpg...). Esto solo las copia a docs/screenshots/
# y avisa de lo que no cuadra, para no subir sin querer imagenes a medias.
#
#   ./tools/importar_capturas.sh                 # busca la SD sola
#   ./tools/importar_capturas.sh /ruta/a/la/sd   # o se la das tu
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESTINO="$REPO/docs/screenshots"

# 1) Localizar el origen
if [ $# -ge 1 ]; then
    ORIGEN="$1/screenshots"
    [ -d "$ORIGEN" ] || ORIGEN="$1"
else
    ORIGEN=""
    for m in /media/"$USER"/*/screenshots /run/media/"$USER"/*/screenshots; do
        [ -d "$m" ] && ORIGEN="$m" && break
    done
fi

if [ -z "${ORIGEN:-}" ] || [ ! -d "$ORIGEN" ]; then
    echo "No encuentro las capturas."
    echo "Mete la tarjeta de la pantalla en el PC, o pasame la ruta:"
    echo "    $0 /media/$USER/NOMBRE_DE_LA_TARJETA"
    exit 1
fi

n=$(find "$ORIGEN" -maxdepth 1 -name '*.jpg' | wc -l)
if [ "$n" -eq 0 ]; then
    echo "En $ORIGEN no hay ninguna captura (.jpg)."
    echo "Lanzalas desde la pantalla: Ajustes -> Display -> interruptor de capturas."
    exit 1
fi

echo "Origen : $ORIGEN  ($n capturas)"
echo "Destino: $DESTINO"

mkdir -p "$DESTINO"
# Borrar SOLO las capturas del carrusel (patron NN_nombre.jpg), no todo el .jpg
# de la carpeta: aqui viven tambien fotos del hardware (max485_en_ne187.jpg,
# photo_*.jpg) que no tienen nada que ver y que un rm *.jpg se llevaba por
# delante. Pasó de verdad el 26-07-2026.
find "$DESTINO" -maxdepth 1 -regextype posix-extended \
     -regex '.*/[0-9]{2}_.*\.jpg' -delete
cp "$ORIGEN"/*.jpg "$DESTINO"/

# 2) Avisos utiles, sin bloquear
echo
vacias=$(find "$DESTINO" -name '*.jpg' -size -8k | wc -l)
[ "$vacias" -gt 0 ] && echo "AVISO: $vacias captura(s) sospechosamente pequenas (<8 KB), revisalas."

# Nombres repetidos = paginas del tour sin nombre propio en TOUR_SET_NAMES.
repes=$(for f in "$DESTINO"/*.jpg; do basename "$f" .jpg | cut -d_ -f2-; done | sort | uniq -d)
if [ -n "$repes" ]; then
    echo "AVISO: nombres repetidos ->" $repes
    echo "       son paginas de Ajustes sin nombre en TOUR_SET_NAMES (main/ui.c)."
fi

echo
echo "Listas $(find "$DESTINO" -name '*.jpg' | wc -l) capturas en docs/screenshots/"
du -sh "$DESTINO"
echo
echo "Para publicarlas:"
echo "    git add docs/screenshots && git commit -m 'docs: capturas de pantalla al dia'"
