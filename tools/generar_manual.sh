#!/usr/bin/env bash
# Genera el PDF del manual a partir de docs/manual/manual-pantalla.md
#
# El manual se escribe en Markdown (se lee bien en GitHub y se versiona); esto lo
# convierte a PDF. Si cambian las capturas o el texto, se relanza y ya.
#
#   ./tools/generar_manual.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MD="$REPO/docs/manual/manual-pantalla.md"
HTML="$REPO/docs/manual/manual-pantalla.html"
PDF="$REPO/docs/manual/manual-pantalla.pdf"

[ -f "$MD" ] || { echo "No encuentro $MD"; exit 1; }

echo "[manual] Markdown -> HTML"
python3 - "$MD" "$HTML" "$REPO/docs/manual" <<'PY'
import sys, os, markdown
md_path, html_path, base = sys.argv[1], sys.argv[2], sys.argv[3]
cuerpo = markdown.markdown(open(md_path, encoding='utf-8').read(),
                          extensions=['tables', 'attr_list'])
# Las imagenes se REDIMENSIONAN a un fichero temporal y se referencian ahi.
# LibreOffice ignora tanto el "width" de la hoja de estilo como el atributo del
# <img>: coloca la captura a su tamano real (1024 px ~ 27 cm) y se sale del A4,
# cortada por la derecha. Escalarlas antes es lo unico que funciona, y de paso
# baja bastante el peso del PDF. 640 px ~ 17 cm = ancho util con estos margenes.
import re, tempfile
from PIL import Image
tmp = tempfile.mkdtemp(prefix='manual_img_')
def escala(m):
    rel = m.group(1)
    src = os.path.normpath(os.path.join(base, rel))
    if not os.path.isfile(src):
        print("   AVISO: falta la imagen", rel)
        return m.group(0)
    dst = os.path.join(tmp, os.path.basename(src))
    im = Image.open(src).convert('RGB')
    im.thumbnail((640, 640 * im.size[1] // im.size[0]), Image.LANCZOS)
    im.save(dst, 'JPEG', quality=88)
    return 'src="%s"' % dst
cuerpo = re.sub(r'src="([^"]+)"', escala, cuerpo)
estilo = """
@page { size: A4; margin: 1.8cm 1.6cm; }
body  { font-family: 'DejaVu Sans', sans-serif; font-size: 10.5pt; line-height: 1.45;
        color: #1a1a1a; }
h1    { font-size: 22pt; color: #0b3d5c; border-bottom: 3px solid #4FC3F7;
        padding-bottom: 6px; }
h2    { font-size: 15pt; color: #0b3d5c; margin-top: 22px;
        border-bottom: 1px solid #cfd8dc; padding-bottom: 3px; }
h3    { font-size: 12pt; color: #24608a; margin-top: 16px; }
img   { width: 15.5cm; border: 1px solid #b0bec5; margin: 8px 0; }
table { border-collapse: collapse; width: 100%; margin: 10px 0; }
th    { background: #eceff1; text-align: left; }
th, td{ border: 1px solid #b0bec5; padding: 5px 8px; font-size: 10pt; }
blockquote { border-left: 4px solid #FFBB33; background: #fff8e6;
             margin: 10px 0; padding: 8px 12px; }
code  { background: #eceff1; padding: 1px 4px; font-family: 'DejaVu Sans Mono', monospace; }
hr    { border: 0; border-top: 1px solid #cfd8dc; margin: 18px 0; }
"""
open(html_path, 'w', encoding='utf-8').write(
    f"<html><head><meta charset='utf-8'><style>{estilo}</style></head>"
    f"<body>{cuerpo}</body></html>")
print("   imagenes:", cuerpo.count('<img'))
PY

echo "[manual] HTML -> PDF (LibreOffice)"
soffice --headless --convert-to pdf --outdir "$REPO/docs/manual" "$HTML" >/dev/null 2>&1
rm -f "$HTML"

[ -f "$PDF" ] || { echo "[manual] ERROR: no se genero el PDF"; exit 1; }
echo "[manual] listo: $PDF  ($(du -h "$PDF" | cut -f1))"
