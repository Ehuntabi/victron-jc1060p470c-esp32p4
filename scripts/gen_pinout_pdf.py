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
import shutil
import subprocess
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
HTML = RAIZ / "docs" / "pinout_guition_jc1060p470c_i.html"
PDF = RAIZ / "docs" / "pinout_guition_jc1060p470c_i.pdf"
VENV = Path.home() / "joint" / "manual" / ".venv" / "bin" / "python3"


def con_weasyprint():
    from weasyprint import HTML as WHTML
    WHTML(filename=str(HTML), base_url=str(HTML.parent)).write_pdf(str(PDF))
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
