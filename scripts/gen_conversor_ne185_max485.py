#!/usr/bin/env python3
"""Plano del conversor NE185 -> MAX485 sobre placa de tiras (30 x 14).

Dibuja en limpio el croquis a mano del 18-sep-2026: la placa que releva al NE187
como maestro del bus RS-485 y le lleva al 7" (Guition) los hilos del NE185 mas la
polarizacion (bias) del bus.

  Uso:
      python3 scripts/gen_conversor_ne185_max485.py

  Salida:
      docs/conversor_ne185_max485.pdf   (una pagina A4)
      docs/conversor_ne185_max485.png   (la misma, para incrustar)

Diseño (croquis del usuario + docs/ne185_bias_board.pdf):

  · Las tiras de cobre van VERTICALES: cada columna es un net.
      A = +12 V (del mazo del NE185)     D = GND
      B = A del bus (diferencial +)      E = +5 V (del 7", pin 1 del J4)
      C = B del bus (diferencial -)      F..N = libres
  · Mazo del NE185 en las filas 1-2 (A, B, C, D).
  · R 120 Ω entre A y B  -> terminacion del bus.
  · R 680 Ω de +5 V a A  y  R 680 Ω de GND a B  -> bias en reposo (lo que hacia el
    NE187 y que el P4 no puede dar solo).
  · Al 7" (J4/J5) en las filas 6-7, en el orden del conector: pin 1 +5 V (rojo),
    pin 2 A (verde), pin 3 B (blanco), pin 4 GND (negro).
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle

RAIZ = Path(__file__).resolve().parent.parent
DOCS = RAIZ / "docs"
PDF = DOCS / "conversor_ne185_max485.pdf"
PNG = DOCS / "conversor_ne185_max485.png"

# ── colores ────────────────────────────────────────────────────────────────
C_FR4 = "#2f6b4f"
C_TIRA = "#f0c9a0"
C_HOLE = "#f7f4ef"
C_TXT = "#1b2733"
C_RAIL = "#0e3a5c"
C_GRIS = "#5b6b7c"
C_12V = "#d63b3b"
C_A = "#2e9e4f"
C_B = "#8a8f98"
C_GND = "#1a1a1a"
C_5V = "#e07b00"
C_RES = "#24557f"

COLUMNAS = "NMLKJIHGFEDCBA"      # como la plantilla: A es la columna de la derecha
FILAS = 30
NETS = {                         # columna -> (net, color)
    "A": ("+12 V", C_12V),
    "B": ("A", C_A),
    "C": ("B", C_B),
    "D": ("GND", C_GND),
    "E": ("+5 V", C_5V),
}


def x_de(letra):
    """Columna (0..13) de una letra. En la plantilla las letras van N..A de
    izquierda a derecha, o sea que A es la columna de la DERECHA: la x de una
    letra es su indice en COLUMNAS (restarlo espejaba la placa)."""
    return COLUMNAS.index(letra)


def dibuja_placa(ax):
    cols = len(COLUMNAS)
    ax.add_patch(FancyBboxPatch((-0.75, -0.75), cols - 1 + 1.5, FILAS - 1 + 1.5,
                                boxstyle="round,pad=0.05", facecolor=C_FR4,
                                edgecolor="#1d4433", linewidth=1.1, zorder=1))
    for c, letra in enumerate(COLUMNAS):
        usado = letra in NETS
        ax.add_patch(Rectangle((c - 0.28, -0.35), 0.56, FILAS - 1 + 0.7,
                               facecolor="#f8e0c2" if usado else C_TIRA,
                               edgecolor="none", zorder=2))
    for f in range(FILAS):
        for c in range(cols):
            ax.add_patch(Circle((c, f), 0.17, facecolor=C_HOLE,
                                edgecolor="#6b6b6b", linewidth=0.35, zorder=3))
    for c, letra in enumerate(COLUMNAS):
        usado = letra in NETS
        for y in (-1.15, FILAS - 1 + 1.15):
            ax.text(c, y, letra, ha="center", va="center", fontsize=8, zorder=5,
                    fontweight="bold" if usado else "normal",
                    color=C_RAIL if usado else "#8a94a0")
    for f in range(FILAS):
        num = f + 1
        usado = num in (1, 2, 4, 5, 6, 7)
        ax.text(-1.05, f, str(num), ha="right", va="center", fontsize=6.4,
                zorder=5, fontweight="bold" if usado else "normal",
                color=C_RAIL if usado else "#93a0ac")
    for letra, (net, color) in NETS.items():
        ax.text(x_de(letra), -2.5, net, ha="center", va="center", fontsize=7.6,
                rotation=90, color="white", fontweight="bold", zorder=8,
                bbox=dict(facecolor=color, edgecolor="none",
                          boxstyle="round,pad=0.22"))
    ax.set_xlim(-1.9, cols - 1 + 5.0)
    ax.set_ylim(FILAS - 1 + 1.7, -3.6)
    ax.set_aspect("equal")
    ax.set_axis_off()


def pad(ax, x, y, color):
    ax.add_patch(Circle((x, y), 0.29, facecolor=color, edgecolor="#1b2733",
                        linewidth=0.6, zorder=7))


def conector(ax, letras, filas, titulo, color):
    xs = [x_de(l) for l in letras]
    y0, y1 = min(filas), max(filas)
    ax.add_patch(FancyBboxPatch((min(xs) - 0.4, y0 - 0.4),
                                max(xs) - min(xs) + 0.8, (y1 - y0) + 0.8,
                                boxstyle="round,pad=0.05", facecolor="none",
                                edgecolor=color, linewidth=1.7, zorder=9))
    for x in xs:
        for y in filas:
            pad(ax, x, y, color)
    ym = (y0 + y1) / 2
    ax.annotate(titulo, xy=(max(xs) + 0.5, ym), xytext=(max(xs) + 2.6, ym),
                ha="left", va="center", fontsize=8.2, color=color,
                fontweight="bold", zorder=10,
                arrowprops=dict(arrowstyle="-", color=color, linewidth=1.0))


def resistencia(ax, x1, y1, x2, y2, valor):
    ax.plot([x1, x2], [y1, y2], color=C_RES, linewidth=1.5,
            solid_capstyle="round", zorder=6)
    mx, my = (x1 + x2) / 2, (y1 + y2) / 2
    ancho = max(abs(x2 - x1) * 0.72, 0.7)
    ax.add_patch(FancyBboxPatch((mx - ancho / 2, my - 0.22), ancho, 0.44,
                                boxstyle="round,pad=0.03", facecolor="#f7e2c8",
                                edgecolor=C_RES, linewidth=1.0, zorder=7))
    ax.text(mx, my + 0.40, valor, ha="center", va="bottom", fontsize=6.6,
            color=C_RES, fontweight="bold", zorder=8,
            bbox=dict(facecolor="white", edgecolor="none", pad=0.6))


def tabla(ax, filas, x_cols, anchos, titulo):
    ax.text(0, 1.0, titulo, fontsize=10.5, fontweight="bold", color=C_RAIL,
            transform=ax.transAxes, va="top")
    y = 0.85
    for i, fila in enumerate(filas):
        cabecera = i == 0
        for txt, x in zip(fila, x_cols):
            ax.text(x, y, txt, fontsize=7.7 if cabecera else 8.1,
                    color=C_RAIL if cabecera else C_TXT,
                    fontweight="bold" if cabecera else "normal",
                    transform=ax.transAxes, va="top")
        y -= 0.115 if cabecera else 0.105
        if cabecera:
            ax.plot([0, 1], [y + 0.03, y + 0.03], color="#c9d8e4",
                    linewidth=0.8, transform=ax.transAxes, clip_on=False)


def main():
    fig = plt.figure(figsize=(11.69, 8.27))          # A4 apaisado
    fig.suptitle("Conversor NE185 → MAX485", fontsize=17, fontweight="bold",
                 color=C_RAIL, y=0.965, x=0.5)
    fig.text(0.5, 0.925,
             "Placa de tiras 30 × 14 (pitch 2,54 mm) · vista cara de componentes, "
             "el cobre va por debajo · releva al NE187 como maestro del bus",
             ha="center", fontsize=8.4, color=C_GRIS)

    ax = fig.add_axes([0.03, 0.05, 0.34, 0.84])
    dibuja_placa(ax)
    conector(ax, ["A", "B", "C", "D"], [0, 1], "Mazo del\nNE185", C_RAIL)
    conector(ax, ["E", "B", "C", "D"], [5, 6], "Al 7\"\n(J4)", "#14567f")
    xA, xB, xC, xD, xE = (x_de(l) for l in "ABCDE")
    resistencia(ax, xB, 3, xC, 3, "120 Ω")
    resistencia(ax, xB, 4, xE, 4, "680 Ω")
    resistencia(ax, xC, 5, xD, 5, "680 Ω")

    ax1 = fig.add_axes([0.40, 0.50, 0.58, 0.38]); ax1.set_axis_off()
    tabla(ax1, [
        ("Cable", "Pin", "Columna", "Nota"),
        ("Mazo del NE185", "+12 V", "A", "solo se trae a la placa"),
        ("(filas 1-2)", "A", "B", "diferencial + del bus"),
        ("", "B", "C", "diferencial − del bus"),
        ("", "GND", "D", "masa común"),
        ("Al 7\" (J4)", "1 · +5 V", "E", "rojo · alimenta el bias"),
        ("(filas 6-7)", "2 · A", "B", "verde"),
        ("", "3 · B", "C", "blanco"),
        ("", "4 · GND", "D", "negro"),
    ], [0.0, 0.26, 0.50, 0.66], None, "Conexiones")

    ax2 = fig.add_axes([0.40, 0.28, 0.58, 0.17]); ax2.set_axis_off()
    tabla(ax2, [
        ("Componente", "Entre", "Para qué"),
        ("R 120 Ω", "B ↔ C", "terminación del bus"),
        ("R 680 Ω", "E ↔ B", "bias: sube A a +5 V"),
        ("R 680 Ω", "D ↔ C", "bias: baja B a GND"),
    ], [0.0, 0.26, 0.50], None, "Componentes")

    ax3 = fig.add_axes([0.40, 0.03, 0.58, 0.21]); ax3.set_axis_off()
    ax3.text(0, 1.0, "Notas", fontsize=10.5, fontweight="bold", color=C_RAIL,
             transform=ax3.transAxes, va="top")
    notas = [
        "· Cada columna es una tira de cobre: A–E son los cinco rieles; las columnas F–N quedan libres.",
        "· El +5 V sale del propio 7\", así el bias está aunque el NE185 esté apagado; el +12 V solo se trae.",
        "· Si las patillas de las resistencias no llegan, se puentea con hilo: importa el net, no el agujero.",
        "· A↔A y B↔B van rectos, sin cruzar. El P4 sustituye al NE187 como maestro del bus.",
    ]
    y = 0.85
    for l in notas:
        ax3.text(0, y, l, fontsize=8.2, color=C_TXT, transform=ax3.transAxes,
                 va="top")
        y -= 0.118

    fig.text(0.5, 0.012,
             "Del croquis a mano del 18-sep-2026 · generado con "
             "scripts/gen_conversor_ne185_max485.py",
             ha="center", fontsize=7.4, color="#8a94a0")

    fig.savefig(PDF)
    fig.savefig(PNG, dpi=170)
    plt.close(fig)
    print("OK -> %s (%d bytes)" % (PDF, PDF.stat().st_size))
    print("OK -> %s (%d bytes)" % (PNG, PNG.stat().st_size))


if __name__ == "__main__":
    main()
