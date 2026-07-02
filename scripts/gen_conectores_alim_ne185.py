#!/usr/bin/env python3
"""Fotos reales anotadas de los conectores 8 y 11 de la placa Guition JC1060P470C_I.

Carga el recorte de la foto real del conector (docs/conectorN_foto.png) y le anade
las etiquetas de cada pin a la derecha, con el orden real leido en la serigrafia:

  - Conector 8  (abajo->arriba): 5V, IO26, IO27, GND   (alim + UART1)
  - Conector 11 (abajo->arriba): 5V, A, B, GND          (RS-485 al NE185)

Genera docs/conector8_alim.png y docs/conector11_j5_ne185.png.
"""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.image as mpimg
from matplotlib.patches import Circle

DOCS = Path(__file__).resolve().parent.parent / "docs"
C_5V, C_BUS, C_GND = "#E01F26", "#1E9BE0", "#1A1A1A"


def annotate(foto, title, right_x, pins, out):
    """pins: lista de arriba->abajo con (y_px, num_pin, nombre, sub, color)."""
    img = mpimg.imread(DOCS / foto)
    h, w = img.shape[:2]
    label_w = 620
    fig, ax = plt.subplots(figsize=((w + label_w) / 100, (h + 90) / 100))
    ax.imshow(img, extent=[0, w, h, 0])
    ax.set_xlim(0, w + label_w)
    ax.set_ylim(h + 55, -55)
    ax.axis("off")
    ax.text((w + label_w) / 2, -32, title, ha="center", va="center",
            fontsize=12.5, fontweight="bold", color="#1A1A1A")
    for y, num, name, sub, color in pins:
        ax.plot([right_x, w + 45], [y, y], color=color, lw=2.6, zorder=5,
                solid_capstyle="round")
        ax.add_patch(Circle((right_x, y), 8, facecolor=color,
                            edgecolor="white", linewidth=1.4, zorder=6))
        ax.text(w + 62, y - 11, f"Pin {num}    {name}", ha="left", va="center",
                fontsize=14, fontweight="bold", color="#1A1A1A")
        ax.text(w + 62, y + 15, sub, ha="left", va="center",
                fontsize=10, color="#555")
    fig.savefig(DOCS / out, dpi=150, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print("escrito", out)


# Conector 8: arriba->abajo = GND(pin4), IO27(pin3), IO26(pin2), 5V(pin1)
annotate("conector8_foto.png",
         "Conector 8 · MX 1.25 4P — alimentacion +5 V (y UART1)",
         252,
         [(122, 4, "GND",  "tierra",        C_GND),
          (188, 3, "IO27", "UART1 RX",      C_BUS),
          (250, 2, "IO26", "UART1 TX",      C_BUS),
          (312, 1, "+5 V", "alimentacion",  C_5V)],
         "conector8_alim.png")

# Conector 11: arriba->abajo = GND(pin4), B(pin3), A(pin2), 5V(pin1)
annotate("conector11_foto.png",
         "Conector 11 · MX 1.25 4P — RS-485 al NE185",
         208,
         [(96,  4, "GND",  "masa del bus",         C_GND),
          (152, 3, "B",    "RS-485 B  →  NE185 B", C_BUS),
          (210, 2, "A",    "RS-485 A  →  NE185 A", C_BUS),
          (268, 1, "+5 V", "alimentacion del bus", C_5V)],
         "conector11_j5_ne185.png")
