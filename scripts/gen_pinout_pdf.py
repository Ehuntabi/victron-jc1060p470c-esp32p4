#!/usr/bin/env python3
"""Regenera docs/pinout_guition_jc1060p470c_i.pdf a partir del HTML.

Uso:
    python3 scripts/gen_pinout_pdf.py
    PY=/ruta/al/python/con/weasyprint python3 scripts/gen_pinout_pdf.py

El HTML (docs/pinout_guition_jc1060p470c_i.html) es la fuente: aquí no se toca
ni una línea de contenido, solo se pasa a PDF.

Motor: WeasyPrint, que es el que permite encabezado con la sección en la que
estás, pie con "Página N de M" y que el <thead> de las tablas se repita al
partir de página. Si en este intérprete no está WeasyPrint se busca el python
del manual (~/joint/manual/.venv) y, si tampoco, se cae a chromium headless
(como se hacía antes: sin encabezado ni pie).
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
HTML = RAIZ / "docs" / "pinout_guition_jc1060p470c_i.html"
PDF = RAIZ / "docs" / "pinout_guition_jc1060p470c_i.pdf"
VENV = Path.home() / "joint" / "manual" / ".venv" / "bin" / "python3"


def prepara(html):
    """Dos retoques de maqueta, ninguno de contenido:
      · a cada <h2> le saca el numero a una etiqueta (<span class="num">) y le pone id,
      · rellena el indice de la portada con esas secciones y su numero de pagina
        (el numero lo pone target-counter() desde el propio PDF)."""
    def h2(m):
        attrs, texto = m.group(1), re.sub(r"\s+", " ", m.group(2)).strip()
        mm = re.match(r"(\d+)\.\s+(.*)$", texto)
        if not mm:
            return m.group(0)
        return '<h2%s id="sec-%s"><span class="num">%s</span> %s</h2>' % (
            attrs, mm.group(1), mm.group(1), mm.group(2))

    html = re.sub(r"<h2([^>]*)>(.*?)</h2>", h2, html, flags=re.S)
    entradas = re.findall(
        r'<h2[^>]*id="sec-(\d+)"[^>]*><span class="num">\d+</span>(.*?)</h2>',
        html, re.S)
    filas = "".join(
        '<div class="linea"><span class="n">%s</span>'
        '<a class="t" href="#sec-%s">%s</a>'
        '<a class="p" href="#sec-%s"></a></div>'
        % (n, n, re.sub(r"<[^>]+>", "", titulo).strip(), n)
        for n, titulo in entradas)
    html = html.replace('<div class="toc" id="toc"></div>',
                        '<div class="toc">%s</div>' % filas)
    return html


def con_weasyprint():
    from weasyprint import HTML as WHTML
    doc = prepara(HTML.read_text(encoding="utf8"))
    WHTML(string=doc, base_url=str(HTML.parent)).write_pdf(str(PDF))
    print("OK (weasyprint) -> %s (%d bytes)" % (PDF, PDF.stat().st_size))


def con_chromium():
    print("AVISO: sin WeasyPrint, tiro de chromium (sin encabezado ni pie)")
    subprocess.run(["chromium", "--headless", "--disable-gpu",
                    "--no-pdf-header-footer", "--print-to-pdf=%s" % PDF,
                    HTML.as_uri()], check=True)
    print("OK (chromium) -> %s (%d bytes)" % (PDF, PDF.stat().st_size))


def main():
    if not HTML.exists():
        sys.exit("No existe %s" % HTML)
    try:
        import weasyprint  # noqa: F401
    except ImportError:
        # ¿nos lo puede hacer el python del manual? (una sola vez)
        if VENV.exists() and os.environ.get("PINOUT_REEXEC") != "1":
            print("WeasyPrint no está aquí; reintento con %s" % VENV)
            os.environ["PINOUT_REEXEC"] = "1"
            os.execv(str(VENV), [str(VENV), str(Path(__file__).resolve())])
        if shutil.which("chromium"):
            con_chromium()
            return
        sys.exit("Ni WeasyPrint ni chromium: no sé con qué generar el PDF.")
    con_weasyprint()


if __name__ == "__main__":
    main()
