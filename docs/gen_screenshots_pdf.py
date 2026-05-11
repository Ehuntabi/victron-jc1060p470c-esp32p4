#!/usr/bin/env python3
"""Recorre todas las vistas Live del firmware via /api/view, captura
/screenshot.bmp de cada una y genera un PDF con todas las pantallas.
Las páginas de Settings se capturan con prompt manual (navegas con touch
y pulsas Enter para capturar).

Uso:
    python3 docs/gen_screenshots_pdf.py [--ip 192.168.4.1]

Requisitos: pip install --user requests fpdf2 Pillow
"""

import argparse
import io
import os
import sys
import time
from datetime import date

import requests
from PIL import Image
from fpdf import FPDF


# ui_view_mode_t (de main/ui/ui_state.h)
LIVE_VIEWS = [
    # (mode_id, slug, titulo_humano)
    (6, "overview",        "Overview"),
    (1, "default_battery", "Default Battery (DC/DC, Batería, Solar)"),
    (3, "battery_monitor", "Battery Monitor (vista completa)"),
    (2, "solar_charger",   "Solar Charger"),
    (5, "dcdc_converter",  "DC/DC Converter"),
    (4, "inverter",        "Inverter"),
]

# Capturas que requieren navegacion manual con el touch
MANUAL_PAGES = [
    ("settings_main",      "Settings — menú principal",
     "Pulsa el icono ⚙ de la barra inferior para abrir Settings."),
    ("settings_display",   "Settings → Display",
     "Entra a Settings → Display (brillo + screensaver + modo nocturno + TZ)."),
    ("settings_wifi",      "Settings → Wi-Fi",
     "Vuelve atrás y entra a Settings → Wi-Fi."),
    ("settings_about",     "Settings → About (Trip + Backup)",
     "Vuelve atrás y entra a Settings → About."),
    ("settings_frigo",     "Settings → Frigo",
     "Vuelve atrás y entra a Settings → Frigo."),
    ("settings_logs",      "Settings → Logs (chart batería)",
     "Vuelve atrás y entra a Settings → Logs → Batería."),
    ("settings_keys",      "Settings → Victron Keys",
     "Vuelve atrás y entra a Settings → Victron Keys."),
    ("settings_sonido",    "Settings → Sonido y avisos",
     "Vuelve atrás y entra a Settings → Sonido y avisos."),
]


def capture(ip: str, out_path: str, retries: int = 3, timeout: int = 8) -> bool:
    """Descarga /screenshot.bmp y la guarda como PNG en out_path."""
    url = f"http://{ip}/screenshot.bmp"
    for attempt in range(retries):
        try:
            r = requests.get(url, timeout=timeout)
            if r.status_code == 200:
                im = Image.open(io.BytesIO(r.content))
                im.save(out_path, optimize=True)
                return True
        except Exception as e:
            print(f"  intento {attempt+1}: {e}")
            time.sleep(1)
    return False


def set_view(ip: str, mode: int) -> bool:
    """Cambia la vista Live activa via POST /api/view?mode=N."""
    url = f"http://{ip}/api/view?mode={mode}"
    try:
        r = requests.post(url, timeout=5)
        return r.status_code == 200
    except Exception as e:
        print(f"  set_view fallo: {e}")
        return False


def build_pdf(captures: list, out_pdf: str) -> None:
    pdf = FPDF(orientation="L", unit="mm", format="A4")
    pdf.set_auto_page_break(auto=True, margin=10)

    font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    bold_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    if os.path.exists(font_path):
        pdf.add_font("DejaVu", "", font_path)
        pdf.add_font("DejaVu", "B", bold_path)
        regular, bold = ("DejaVu", ""), ("DejaVu", "B")
    else:
        regular, bold = ("Helvetica", ""), ("Helvetica", "B")

    # Portada
    pdf.add_page()
    pdf.set_font(*bold, 22)
    pdf.set_text_color(255, 152, 0)
    pdf.ln(50)
    pdf.cell(0, 12, "Capturas de pantalla", align="C",
             new_x="LMARGIN", new_y="NEXT")
    pdf.set_font(*bold, 16)
    pdf.set_text_color(0, 0, 0)
    pdf.cell(0, 10, "VictronSolarDisplay — JC1060P470C_I",
             align="C", new_x="LMARGIN", new_y="NEXT")
    pdf.ln(8)
    pdf.set_font(*regular, 11)
    pdf.set_text_color(110, 110, 110)
    pdf.cell(0, 6, f"Generado: {date.today().isoformat()}",
             align="C", new_x="LMARGIN", new_y="NEXT")

    for title, png in captures:
        pdf.add_page()
        pdf.set_font(*bold, 14)
        pdf.set_text_color(0, 0, 0)
        pdf.cell(0, 8, title, new_x="LMARGIN", new_y="NEXT")
        try:
            pdf.image(png, x=15, y=22, w=267)
        except Exception as e:
            pdf.set_font(*regular, 11)
            pdf.cell(0, 6, f"(error abriendo {png}: {e})",
                     new_x="LMARGIN", new_y="NEXT")
    pdf.output(out_pdf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.4.1")
    ap.add_argument("--out-dir", default="docs/screenshots")
    ap.add_argument("--pdf", default="docs/screenshots.pdf")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="segundos de espera tras cambiar de vista")
    ap.add_argument("--skip-manual", action="store_true",
                    help="capturar solo las vistas Live (sin Settings)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    captures: list = []

    # 1) Vistas Live automatizadas
    print(f"\n=== Capturando vistas Live (host {args.ip}) ===")
    for mode, slug, title in LIVE_VIEWS:
        print(f"\n[mode={mode}] {title}")
        if not set_view(args.ip, mode):
            print("  -> falló set_view, omitiendo")
            continue
        time.sleep(args.settle)
        png = os.path.join(args.out_dir, f"{slug}.png")
        if capture(args.ip, png):
            print(f"  -> {png}")
            captures.append((title, png))
        else:
            print("  -> falló capture")

    # 2) Settings con touch manual
    if not args.skip_manual:
        print(f"\n=== Páginas con navegación manual ===")
        for slug, title, prompt in MANUAL_PAGES:
            print(f"\n>> {title}")
            print(f"   {prompt}")
            input("   Cuando esté listo pulsa ENTER para capturar (o 's' + ENTER para saltar): ")
            # Permitir saltar
            png = os.path.join(args.out_dir, f"{slug}.png")
            if capture(args.ip, png):
                print(f"   -> {png}")
                captures.append((title, png))
            else:
                print("   -> capture falló")

    # 3) Generar PDF
    if not captures:
        print("\nSin capturas; no se genera PDF.")
        return 1
    print(f"\n=== Generando PDF con {len(captures)} páginas ===")
    build_pdf(captures, args.pdf)
    print(f"PDF: {args.pdf}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
