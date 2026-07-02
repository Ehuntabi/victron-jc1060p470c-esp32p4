#!/usr/bin/env python3
"""Diagramas de los conectores fisicos de alimentacion / NE185 de la placa
Guition JC1060P470C_I, tal como estan serigrafiados (orden leido en la placa,
de abajo -> arriba = pin 1 -> pin 4).

Genera dos PNG en docs/:
  - conector8_alim.png      : Conector 8  -> 5V, IO26, IO27, GND (alim + UART1)
  - conector11_j5_ne185.png : Conector 11 -> 5V, A, B, GND      (J5 RS-485 al NE185)

Estilo visual coherente con docs/cn2_pinout.png (cajas redondeadas de colores).
"""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

DOCS = Path(__file__).resolve().parent.parent / "docs"

C_5V  = "#D32F2F"   # rojo  -> +5V
C_BUS = "#29B6F6"   # azul  -> datos (A/B o UART1)
C_GND = "#1A1A1A"   # negro -> GND
TXT_ON_DARK = "white"


def draw(pins, title, outpath):
    """pins: lista pin1..pinN de abajo->arriba, cada (numero, nombre, sub, color)."""
    n = len(pins)
    fig, ax = plt.subplots(figsize=(6.4, 0.95 * n + 1.15))
    ax.set_xlim(0, 10)
    ax.set_ylim(0, n + 1.15)
    ax.axis("off")

    ax.text(5, n + 0.72, title, ha="center", va="center",
            fontsize=13, fontweight="bold", color="#1A1A1A")
    ax.text(0.25, (n - 1) + 0.48, "pin 4\n(arriba)", ha="center", va="center",
            fontsize=7.5, color="#888", rotation=90)

    # pin1 abajo (y=0) ... pinN arriba
    for i, (num, name, sub, color) in enumerate(pins):
        y = i
        box = FancyBboxPatch((0.9, y + 0.08), 8.2, 0.80,
                             boxstyle="round,pad=0.02,rounding_size=0.12",
                             facecolor=color, edgecolor="#555", linewidth=1.2)
        ax.add_patch(box)
        ax.text(1.35, y + 0.48, f"Pin {num}", ha="left", va="center",
                fontsize=9, fontweight="bold", color=TXT_ON_DARK)
        ax.text(5.1, y + 0.55, name, ha="center", va="center",
                fontsize=14, fontweight="bold", color=TXT_ON_DARK)
        if sub:
            ax.text(5.1, y + 0.24, sub, ha="center", va="center",
                    fontsize=8.5, color=TXT_ON_DARK)
    ax.text(0.25, 0.48, "pin 1\n(abajo)", ha="center", va="center",
            fontsize=7.5, color="#888", rotation=90)

    fig.tight_layout(pad=0.4)
    fig.savefig(outpath, dpi=200, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print("escrito", outpath)


# Conector 8 (abajo -> arriba): 5V, IO26, IO27, GND
draw([
    (1, "+5 V",  "entrada de alimentacion",           C_5V),
    (2, "IO26",  "UART1 TX (comparte con MAX485/NE185)", C_BUS),
    (3, "IO27",  "UART1 RX (comparte con MAX485/NE185)", C_BUS),
    (4, "GND",   "tierra",                              C_GND),
], "Conector 8 - MX 1.25 4P (alimentacion +5V + UART1)",
   DOCS / "conector8_alim.png")

# Conector 11 / J5 (abajo -> arriba): 5V, A, B, GND
draw([
    (1, "+5 V", "alimentacion del bus",  C_5V),
    (2, "A",    "RS-485 A  ->  NE185 A", C_BUS),
    (3, "B",    "RS-485 B  ->  NE185 B", C_BUS),
    (4, "GND",  "masa comun del bus",    C_GND),
], "Conector 11 / J5 - MX 1.25 4P (RS-485 al NE185)",
   DOCS / "conector11_j5_ne185.png")
