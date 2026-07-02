#!/usr/bin/env python3
"""Diagramas de los conectores fisicos de alimentacion / NE185 de la placa
Guition JC1060P470C_I.

Dibuja el conector MX 1.25 4P tal como esta montado en la placa:
  - vista superior, ABERTURA (salida de cables) hacia la IZQUIERDA
  - los 4 pines en columna, orden real leido en la serigrafia:
    pin 1 = abajo ... pin 4 = arriba
  - etiqueta de cada pin a la DERECHA

Genera dos PNG en docs/:
  - conector8_alim.png      : Conector 8  -> 5V, IO26, IO27, GND (alim + UART1)
  - conector11_j5_ne185.png : Conector 11 -> 5V, A, B, GND      (J5 RS-485 al NE185)
"""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle, Circle

DOCS = Path(__file__).resolve().parent.parent / "docs"

C_5V  = "#D32F2F"   # rojo  -> +5V
C_BUS = "#1E9BE0"   # azul  -> datos (A/B o UART1)
C_GND = "#1A1A1A"   # negro -> GND
HOUSING = "#EFECE4"  # blanco hueso del conector MX 1.25
HOUSE_EDGE = "#B7B0A0"


def draw(pins, title, outpath):
    """pins: lista pin1..pin4 de abajo->arriba, cada (numero, nombre, sub, color)."""
    n = len(pins)
    fig, ax = plt.subplots(figsize=(7.4, 4.7))
    ax.set_xlim(0, 15)
    ax.set_ylim(0, 8.4)
    ax.axis("off")

    ax.text(7.5, 8.0, title, ha="center", va="center",
            fontsize=12.5, fontweight="bold", color="#1A1A1A")
    ax.text(7.5, 7.35, "vista superior · abertura (cables) a la izquierda · pin 1 abajo",
            ha="center", va="center", fontsize=8.5, color="#666", style="italic")

    # --- cuerpo del conector (housing MX 1.25) ---
    hx0, hx1 = 3.2, 6.2
    hy0, hy1 = 0.9, 6.5
    house = FancyBboxPatch((hx0, hy0), hx1 - hx0, hy1 - hy0,
                           boxstyle="round,pad=0.02,rounding_size=0.18",
                           facecolor=HOUSING, edgecolor=HOUSE_EDGE, linewidth=1.6)
    ax.add_patch(house)
    # pared trasera (derecha) mas marcada
    ax.add_patch(Rectangle((hx1 - 0.35, hy0 + 0.1), 0.35, (hy1 - hy0) - 0.2,
                           facecolor="#DAD4C6", edgecolor="none"))
    # "boca" / abertura en la cara izquierda
    ax.add_patch(Rectangle((hx0, hy0 + 0.25), 0.22, (hy1 - hy0) - 0.5,
                           facecolor="#8A8272", edgecolor="none"))
    ax.text(2.9, hy1 + 0.15, "abertura", ha="right", va="bottom",
            fontsize=8, color="#888")

    # posiciones de los 4 pines en columna (pin1 abajo)
    ys = [hy0 + (hy1 - hy0) * (i + 0.5) / n for i in range(n)]
    pin_cx = (hx0 + hx1) / 2 + 0.15

    for (num, name, sub, color), y in zip(pins, ys):
        # cable saliendo a la IZQUIERDA (color de la senal)
        ax.plot([hx0 - 1.7, hx0 + 0.15], [y, y], color=color, linewidth=3.2,
                solid_capstyle="round", zorder=1)
        # contacto/pin sobre el housing
        ax.add_patch(Circle((pin_cx, y), 0.34, facecolor=color,
                            edgecolor="white", linewidth=1.6, zorder=3))
        ax.text(pin_cx, y, str(num), ha="center", va="center",
                fontsize=9, fontweight="bold", color="white", zorder=4)
        # linea guia a la etiqueta (derecha)
        ax.plot([pin_cx + 0.34, 8.4], [y, y], color="#BBB", linewidth=1.0, zorder=1)
        ax.add_patch(Circle((8.4, y), 0.12, facecolor=color, edgecolor="none", zorder=3))
        # etiqueta
        ax.text(8.8, y + 0.16, f"Pin {num}   {name}", ha="left", va="center",
                fontsize=12, fontweight="bold", color="#1A1A1A")
        ax.text(8.8, y - 0.28, sub, ha="left", va="center",
                fontsize=8.5, color="#555")

    ax.text(hx0 - 1.7, ys[0] - 0.55, "cables ↞", ha="left", va="center",
            fontsize=8, color="#888")

    fig.savefig(outpath, dpi=200, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print("escrito", outpath)


# Conector 8 (abajo -> arriba): 5V, IO26, IO27, GND
draw([
    (1, "+5 V", "entrada de alimentacion",              C_5V),
    (2, "IO26", "UART1 TX (comparte con MAX485/NE185)", C_BUS),
    (3, "IO27", "UART1 RX (comparte con MAX485/NE185)", C_BUS),
    (4, "GND",  "tierra",                               C_GND),
], "Conector 8 · MX 1.25 4P — alimentacion +5 V (y UART1)",
   DOCS / "conector8_alim.png")

# Conector 11 / J5 (abajo -> arriba): 5V, A, B, GND
draw([
    (1, "+5 V", "alimentacion del bus",  C_5V),
    (2, "A",    "RS-485 A  →  NE185 A",  C_BUS),
    (3, "B",    "RS-485 B  →  NE185 B",  C_BUS),
    (4, "GND",  "masa comun del bus",    C_GND),
], "Conector 11 / J5 · MX 1.25 4P — RS-485 al NE185",
   DOCS / "conector11_j5_ne185.png")
