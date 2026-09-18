#!/usr/bin/env python3
"""Plano del conversor NE185 -> MAX485 sobre placa de tiras (30 x 14).

Version 2: la maqueta se calcula en PULGADAS (nada de posiciones a ojo) y la placa
solo lleva marcas numeradas (1..5); los textos van en las tablas de la derecha, asi
que no hay nada que se pise. Los dos conectores son de UNA fila de pines (4 hilos
cada uno), no de dos.

  Uso:    python3 scripts/gen_conversor_ne185_max485.py
  Salida: docs/conversor_ne185_max485.pdf  y  .png  (A4 apaisado)

Diseño (croquis a mano del 18-sep-2026 + docs/ne185_bias_board.pdf):

  · Las tiras de cobre van VERTICALES: cada columna es un net. En la plantilla las
    letras van N..A de izquierda a derecha, o sea que A es la columna de la DERECHA.
      A = +12 V (mazo del NE185)      D = GND
      B = A del bus (diferencial +)   E = +5 V (del 7", pin 1 del J4)
      C = B del bus (diferencial -)   F..N = libres
  · (1) Mazo del NE185, fila 1 (A, B, C, D).
  · (2) Al 7" (J4), fila 7 (E, B, C, D), en el orden del conector: pin 1 +5 V (rojo),
        pin 2 A (verde), pin 3 B (blanco), pin 4 GND (negro).
  · (3) R 120 Ω entre A y B, fila 4  -> terminacion del bus.
  · (4) R 680 Ω de +5 V a A, fila 5  -> bias (sube).
  · (5) R 680 Ω de GND a B, fila 6   -> bias (baja). Queda DEBAJO del conector (2),
        que se monta encima: primero se suelda la resistencia y luego el conector.
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
C_FR4 = "#2e6b4f"
C_TIRA = "#f0c9a0"
C_TIRA_USADA = "#f9dfbe"
C_HOLE = "#f8f5f0"
C_TXT = "#1b2733"
C_RAIL = "#0e3a5c"
C_GRIS = "#5b6b7c"
C_12V = "#d63b3b"
C_A = "#2e9e4f"
C_B = "#8a8f98"
C_GND = "#1a1a1a"
C_5V = "#e07b00"
C_RES = "#24557f"

COLUMNAS = "NMLKJIHGFEDCBA"      # N a la izquierda, A a la derecha (plantilla)
FILAS = 30
NETS = {"A": ("+12 V", C_12V), "B": ("A", C_A), "C": ("B", C_B),
        "D": ("GND", C_GND), "E": ("+5 V", C_5V)}
FILAS_MARCADAS = (1, 4, 5, 6, 7)


def x_de(letra):
    """Columna (0..13) de una letra: A es la de la derecha (indice en COLUMNAS)."""
    return COLUMNAS.index(letra)


# ════════════════════════════════════════════════════════════════════════════
# la placa
# ════════════════════════════════════════════════════════════════════════════

def dibuja_placa(ax):
    cols = len(COLUMNAS)
    ax.add_patch(FancyBboxPatch((-0.75, -0.75), cols - 1 + 1.5, FILAS - 1 + 1.5,
                                boxstyle="round,pad=0.04", facecolor=C_FR4,
                                edgecolor="#1d4433", linewidth=1.0, zorder=1))
    for c, letra in enumerate(COLUMNAS):
        color = C_TIRA_USADA if letra in NETS else C_TIRA
        ax.add_patch(Rectangle((c - 0.27, -0.34), 0.54, FILAS - 1 + 0.68,
                               facecolor=color, edgecolor="none", zorder=2))
        if letra in NETS:                      # tinte del net, para verlo de un vistazo
            ax.add_patch(Rectangle((c - 0.27, -0.34), 0.54, FILAS - 1 + 0.68,
                                   facecolor=NETS[letra][1], alpha=0.30,
                                   edgecolor="none", zorder=2.5))
    for f in range(FILAS):
        for c in range(cols):
            ax.add_patch(Circle((c, f), 0.16, facecolor=C_HOLE,
                                edgecolor="#707070", linewidth=0.3, zorder=3))
    for c, letra in enumerate(COLUMNAS):
        usado = letra in NETS
        for y in (-1.05, FILAS - 1 + 1.05):
            ax.text(c, y, letra, ha="center", va="center", fontsize=7.6, zorder=5,
                    fontweight="bold" if usado else "normal",
                    color=C_RAIL if usado else "#93a0ac")
    for f in range(FILAS):
        num = f + 1
        usado = num in FILAS_MARCADAS
        ax.text(-0.85, f, str(num), ha="right", va="center", fontsize=6.0,
                zorder=5, fontweight="bold" if usado else "normal",
                color=C_RAIL if usado else "#98a4b0")
    ax.set_xlim(-3.4, cols - 1 + 1.0)
    ax.set_ylim(FILAS - 1 + 1.6, -1.9)
    ax.set_aspect("equal")
    ax.set_axis_off()


def pad(ax, x, y, color):
    ax.add_patch(Circle((x, y), 0.28, facecolor=color, edgecolor="#152332",
                        linewidth=0.5, zorder=7))


def marca(ax, x, y, numero, color):
    """Marca numerada, para no escribir texto encima de la placa."""
    ax.add_patch(Circle((x, y), 0.52, facecolor=color, edgecolor="white",
                        linewidth=1.0, zorder=14))
    ax.text(x, y, str(numero), ha="center", va="center", fontsize=7.4,
            color="white", fontweight="bold", zorder=15)


def conector(ax, letras, fila, numero, color):
    """Conector de UNA fila de pines (4 hilos) sobre los agujeros que ocupa."""
    xs = [x_de(l) for l in letras]
    ax.add_patch(FancyBboxPatch((min(xs) - 0.34, fila - 0.34),
                                max(xs) - min(xs) + 0.68, 0.68,
                                boxstyle="round,pad=0.04", facecolor=color,
                                alpha=0.12, edgecolor=color, linewidth=1.6,
                                zorder=9))
    for x in xs:
        pad(ax, x, fila, color)
    marca(ax, min(xs) - 1.35, fila, numero, color)


def resistencia(ax, x1, x2, fila, numero, debajo=False):
    estilo = (0, (2.6, 1.8)) if debajo else "solid"
    z = 12 if debajo else 6
    ax.plot([x1, x2], [fila, fila], color=C_RES, linewidth=1.4, zorder=z,
            linestyle=estilo, solid_capstyle="round")
    mx = (x1 + x2) / 2
    ancho = max(abs(x2 - x1) * 0.70, 0.66)
    ax.add_patch(FancyBboxPatch((mx - ancho / 2, fila - 0.21), ancho, 0.42,
                                boxstyle="round,pad=0.03",
                                facecolor="#fdf1e0" if debajo else "#f7e2c8",
                                edgecolor=C_RES, linewidth=0.9, zorder=z + 1,
                                linestyle=estilo))
    marca(ax, min(x1, x2) - 1.35, fila, numero, C_RES)


# ════════════════════════════════════════════════════════════════════════════
# paneles de texto
# ════════════════════════════════════════════════════════════════════════════

def tabla(ax, filas, x_cols, titulo, tam=7.9):
    ax.set_axis_off()
    ax.text(0, 1.0, titulo, fontsize=11, fontweight="bold", color=C_RAIL,
            transform=ax.transAxes, va="top")
    y = 0.80
    for i, fila in enumerate(filas):
        cab = i == 0
        for txt, x in zip(fila, x_cols):
            ax.text(x, y, txt, fontsize=tam if not cab else tam - 0.3,
                    color=C_RAIL if cab else C_TXT,
                    fontweight="bold" if cab else "normal",
                    transform=ax.transAxes, va="top")
        y -= (0.115 if cab else 0.105)
        if cab:
            ax.plot([0, 1], [y + 0.035, y + 0.035], color="#c9d8e4",
                    linewidth=0.8, transform=ax.transAxes, clip_on=False)


def main():
    L, A = 11.69, 8.27                       # A4 apaisado, en pulgadas
    fig = plt.figure(figsize=(L, A))

    def caja(x, y, ancho, alto):
        """Rectangulo en pulgadas -> fraccion de figura."""
        return [x / L, y / A, ancho / L, alto / A]

    fig.text(0.5, 1 - 0.42 / A, "Conversor NE185 → MAX485", ha="center",
             va="center", fontsize=18, fontweight="bold", color=C_RAIL)
    fig.text(0.5, 1 - 0.72 / A,
             "Placa de tiras 30 × 14 (pitch 2,54 mm) · vista cara de componentes, "
             "el cobre va por debajo · releva al NE187 como maestro del bus",
             ha="center", va="center", fontsize=8.6, color=C_GRIS)

    ax = fig.add_axes(caja(0.25, 0.45, 3.95, 6.95))
    dibuja_placa(ax)

    xA, xB, xC, xD, xE = (x_de(l) for l in "ABCDE")
    conector(ax, ["A", "B", "C", "D"], 0, 1, C_RAIL)              # fila 1
    conector(ax, ["E", "B", "C", "D"], 6, 2, "#14567f")           # fila 7
    resistencia(ax, xB, xC, 3, 3)                                 # fila 4
    resistencia(ax, xB, xE, 4, 4)                                 # fila 5
    resistencia(ax, xC, xD, 5, 5, debajo=True)                    # fila 6

    tabla(fig.add_axes(caja(4.55, 4.70, 6.85, 2.62)),
          [("Cable", "Pin", "Columna", "Nota"),
           ("Mazo del NE185 (1) · fila 1", "+12 V", "A", "solo se trae a la placa"),
           ("", "A", "B", "diferencial + del bus"),
           ("", "B", "C", "diferencial − del bus"),
           ("", "GND", "D", "masa común"),
           ("Al 7\" · J4 (2) · fila 7", "1 · +5 V", "E", "rojo · alimenta el bias"),
           ("", "2 · A", "B", "verde"),
           ("", "3 · B", "C", "blanco"),
           ("", "4 · GND", "D", "negro")],
          [0.0, 0.28, 0.46, 0.60], "Conexiones")

    tabla(fig.add_axes(caja(4.55, 3.32, 6.85, 1.30)),
          [("Componente", "Entre", "Para qué"),
           ("(3)  R 120 Ω · fila 4", "B ↔ C", "terminación del bus"),
           ("(4)  R 680 Ω · fila 5", "E ↔ B", "bias: sube A a +5 V"),
           ("(5)  R 680 Ω · fila 6", "D ↔ C", "bias: baja B a GND · va DEBAJO del conector (2)")],
          [0.0, 0.31, 0.53], "Componentes")

    ax3 = fig.add_axes(caja(4.55, 0.45, 6.85, 2.80)); ax3.set_axis_off()
    ax3.text(0, 1.0, "Nets y notas", fontsize=11, fontweight="bold",
             color=C_RAIL, transform=ax3.transAxes, va="top")
    lineas = [
        (C_12V, "Columna A = +12 V del mazo del NE185 · solo se trae, el MAX485 no lo usa"),
        (C_A, "Columna B = A del bus RS-485 (diferencial +)"),
        (C_B, "Columna C = B del bus RS-485 (diferencial −)"),
        (C_GND, "Columna D = GND"),
        (C_5V, "Columna E = +5 V del propio 7\" (pin 1 del J4): así el bias está aunque el NE185 esté apagado"),
        (None, "Cada columna es una tira de cobre; las columnas F–N quedan libres."),
        (None, "Los dos conectores son de una fila de pines (4 hilos cada uno). El del J4 se monta"),
        (None, "ENCIMA de la R (5): se suelda primero la resistencia y luego el conector."),
        (None, "A↔A y B↔B van rectos, sin cruzar. Si no llegan las patillas, se puentea con hilo."),
    ]
    y = 0.84
    for color, txt in lineas:
        if color:
            ax3.plot([0.0, 0.022], [y + 0.012, y + 0.012], color=color,
                     linewidth=3.0, transform=ax3.transAxes, clip_on=False)
        ax3.text(0.045 if color else 0.0, y, txt, fontsize=7.9, color=C_TXT,
                 transform=ax3.transAxes, va="top")
        y -= 0.105

    fig.text(0.5, 0.10 / A,
             "Del croquis a mano del 18-sep-2026 · generado con "
             "scripts/gen_conversor_ne185_max485.py",
             ha="center", va="center", fontsize=7.2, color="#98a4b0")

    fig.savefig(PDF)
    fig.savefig(PNG, dpi=170)
    plt.close(fig)
    print("OK -> %s (%d bytes)" % (PDF, PDF.stat().st_size))
    print("OK -> %s (%d bytes)" % (PNG, PNG.stat().st_size))


if __name__ == "__main__":
    main()
