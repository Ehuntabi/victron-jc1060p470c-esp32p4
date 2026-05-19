#!/usr/bin/env python3
"""Genera docs/stripboard_pzem.pdf del 7" Guition JC1060P470C_I.

Uso:
    python3 scripts/gen_stripboard_pdf.py    # escribe docs/stripboard_pzem.pdf

Pág 1 — plantilla en blanco 14 x 30 para bocetar a mano.
Pág 2 — divisor PZEM (10k+20k) sobre placa real 10 x 16, sin cortes de pista
        (cada senial en su propia fila/tira; R1 y R2 conectan filas D-E y E-F).

Mismo backend que wiring_pzem.pdf: matplotlib PdfPages con paginas A4
COMPLETAS (sin bbox_inches='tight', que recorta a tamanio no-estandar y
hace que xreader cuelgue al abrir el PDF).
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Rectangle, Circle, FancyBboxPatch
from matplotlib.lines import Line2D
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs" / "stripboard_pzem.pdf"

# Colores
C_STRIP   = "#FFD8A8"
C_HOLE    = "#FFFFFF"
C_BG      = "#FFF9F2"
C_OUTLINE = "#888888"
C_LABEL   = "#222222"
C_R_BODY  = "#B6BBC2"
C_R_LEAD  = "#2E2E2E"
C_WIRE_R  = "#D32F2F"
C_WIRE_K  = "#1A1A1A"
C_WIRE_Y  = "#FFB300"
C_WIRE_G  = "#388E3C"

def draw_blank(ax, rows, cols, with_labels=True):
    row_labels = [chr(ord("A") + i) for i in range(rows)]
    ax.add_patch(Rectangle((-0.7, -0.7), cols - 1 + 1.4, rows - 1 + 1.4,
                            facecolor=C_BG, edgecolor=C_OUTLINE, linewidth=1.2))
    for r in range(rows):
        ax.add_patch(Rectangle((-0.35, r - 0.30), cols - 1 + 0.7, 0.60,
                                facecolor=C_STRIP, edgecolor="none", zorder=2))
    for r in range(rows):
        for c in range(cols):
            ax.add_patch(Circle((c, r), 0.18, facecolor=C_HOLE,
                                edgecolor="#555", linewidth=0.4, zorder=3))
    if with_labels:
        for r, lbl in enumerate(row_labels):
            ax.text(-1.1, r, lbl, ha="center", va="center",
                    fontsize=8, color=C_LABEL)
            ax.text(cols - 1 + 1.1, r, lbl, ha="center", va="center",
                    fontsize=8, color=C_LABEL)
        for c in range(cols):
            ax.text(c, -1.2, str(c + 1), ha="center", va="center",
                    fontsize=7, color=C_LABEL)
            ax.text(c, rows - 1 + 1.2, str(c + 1), ha="center", va="center",
                    fontsize=7, color=C_LABEL)
    ax.set_xlim(-2.0, cols - 1 + 2.0)
    ax.set_ylim(rows - 1 + 2.0, -2.0)
    ax.set_aspect("equal")
    ax.set_axis_off()

def draw_vertical_resistor(ax, r_top, r_bot, c, value_label):
    ax.add_line(Line2D([c, c], [r_top + 0.25, r_bot - 0.25],
                       color=C_R_LEAD, linewidth=1.2, zorder=5))
    body_h = (r_bot - r_top) - 0.5
    ax.add_patch(FancyBboxPatch((c - 0.18, r_top + 0.25), 0.36, body_h,
                                 boxstyle="round,pad=0,rounding_size=0.06",
                                 facecolor=C_R_BODY, edgecolor="#666",
                                 linewidth=0.6, zorder=6))
    ax.add_patch(Circle((c, r_top), 0.10, facecolor=C_R_LEAD, zorder=7))
    ax.add_patch(Circle((c, r_bot), 0.10, facecolor=C_R_LEAD, zorder=7))
    ax.text(c + 0.55, (r_top + r_bot) / 2, value_label, ha="left", va="center",
            fontsize=9, color=C_LABEL, fontweight="bold", zorder=7)

def draw_pad(ax, r, c, label, color, anchor="left"):
    ax.add_patch(Circle((c, r), 0.28, facecolor=color, edgecolor="black",
                        linewidth=0.6, zorder=7))
    dx = -0.7 if anchor == "left" else 0.7
    ha = "right" if anchor == "left" else "left"
    ax.text(c + dx, r, label, ha=ha, va="center", fontsize=7.5,
            color=C_LABEL, fontweight="bold", zorder=10,
            bbox=dict(facecolor="white", edgecolor=color, pad=2,
                       boxstyle="round,pad=0.2"))

def draw_wire(ax, r, c0, c1, color):
    ax.add_line(Line2D([c0, c1], [r, r], color=color, linewidth=2.4,
                       solid_capstyle="round", zorder=4))


with PdfPages(OUT) as pdf:
    # ── PÁGINA 1: plantilla en blanco 14 x 30 ─────────────────────
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Stripboard 14 x 30 - plantilla en blanco",
                  fontsize=16, fontweight="bold", y=0.95)
    fig.text(0.5, 0.91, "Vista cara componentes (las tiras de cobre van por DEBAJO).  "
                          "Pitch 2.54 mm.  Dibuja aqui tu cableado.",
              ha="center", fontsize=9, color="#555")
    ax = fig.add_axes([0.05, 0.06, 0.90, 0.82])
    draw_blank(ax, rows=14, cols=30)
    pdf.savefig(fig)
    plt.close(fig)

    # ── PÁGINA 2: divisor PZEM en placa 10 x 16 ───────────────────
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Divisor PZEM TX -> ESP RX  (stripboard 10 x 16)",
                  fontsize=16, fontweight="bold", y=0.96)
    fig.text(0.5, 0.92,
              "PZEM saca 5V en su TX; el ESP32-P4 solo tolera 3.3V en RX. "
              "Divisor 10k + 20k -> 5V x 2/3 = 3.33V.",
              ha="center", fontsize=9, color="#555")
    ax = fig.add_axes([0.05, 0.32, 0.90, 0.60])
    ROWS, COLS = 10, 16
    draw_blank(ax, rows=ROWS, cols=COLS)
    DIV_COL = 6
    PAD_R_COL = COLS - 1 - 2

    # +5V (fila B)
    draw_pad(ax, 1, 0, "+5V (IDC pin 2)", C_WIRE_R, anchor="left")
    draw_wire(ax, 1, 0, PAD_R_COL, C_WIRE_R)
    draw_pad(ax, 1, PAD_R_COL, "PZEM VCC", C_WIRE_R, anchor="right")

    # PZEM TX (5V) entra al divisor (fila D)
    draw_pad(ax, 3, 0, "PZEM TX (5V)", C_WIRE_Y, anchor="left")
    draw_wire(ax, 3, 0, DIV_COL, C_WIRE_Y)

    draw_vertical_resistor(ax, 3, 4, DIV_COL, "R1 = 10 kohm")

    # Nodo central -> ESP RX (3.3V) en fila E
    draw_wire(ax, 4, DIV_COL, PAD_R_COL, C_WIRE_R)
    draw_pad(ax, 4, PAD_R_COL, "ESP RX (GPIO 2 / pin 9)\n3.3V", C_WIRE_R,
             anchor="right")

    draw_vertical_resistor(ax, 4, 5, DIV_COL, "R2 = 20 kohm")

    # GND (fila F)
    draw_pad(ax, 5, 0, "GND (IDC pin 5)", C_WIRE_K, anchor="left")
    draw_wire(ax, 5, 0, PAD_R_COL, C_WIRE_K)
    draw_pad(ax, 5, PAD_R_COL, "PZEM GND", C_WIRE_K, anchor="right")

    # ESP TX (3.3V) directo a PZEM RX (fila H)
    draw_pad(ax, 7, 0, "ESP TX (GPIO 1 / pin 7)\n3.3V", C_WIRE_G,
             anchor="left")
    draw_wire(ax, 7, 0, PAD_R_COL, C_WIRE_G)
    draw_pad(ax, 7, PAD_R_COL, "PZEM RX", C_WIRE_G, anchor="right")

    # Leyenda + notas
    notes_ax = fig.add_axes([0.05, 0.04, 0.90, 0.25])
    notes_ax.set_axis_off()
    notes_ax.text(0.0, 1.0, "Leyenda:", fontsize=10, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    legend_y = 0.88
    for color, label in [
        (C_WIRE_R, "Rojo: +5V y ESP RX (salida del divisor, 3.3V)"),
        (C_WIRE_K, "Negro: GND comun"),
        (C_WIRE_G, "Verde: ESP TX -> PZEM RX (3.3V directo, sin divisor)"),
        (C_WIRE_Y, "Amarillo: PZEM TX (5V, entrada al divisor)"),
    ]:
        notes_ax.plot([0.02, 0.07], [legend_y, legend_y], color=color,
                       linewidth=3, transform=notes_ax.transAxes)
        notes_ax.text(0.09, legend_y, label, fontsize=9,
                       transform=notes_ax.transAxes, va="center")
        legend_y -= 0.10

    notes_ax.text(0.0, 0.42, "Notas criticas:", fontsize=10,
                   fontweight="bold", transform=notes_ax.transAxes, va="top")
    note_y = 0.32
    for txt in [
        "1. NO se necesitan cortes de pista: cada senial vive en su propia fila (B/D/E/F/H). R1 y R2 (verticales) son los unicos puentes entre tiras adyacentes (D-E y E-F).",
        "2. GND comun entre PZEM y ESP es OBLIGATORIO (sin el, la UART no funciona).",
        "3. Los dos terminales gordos verdes del PZEM (L y N) van a la red 230V; NUNCA al cable de seniales.",
        "4. Cable de seniales hasta el PZEM: par trenzado / apantallado de 4 hilos (+5V, GND, ESP_TX, ESP_RX). Pantalla a GND solo en el lado ESP.",
    ]:
        notes_ax.text(0.0, note_y, txt, fontsize=8.5,
                       transform=notes_ax.transAxes, va="top",
                       wrap=True)
        note_y -= 0.10

    pdf.savefig(fig)
    plt.close(fig)

print(f"OK -> {OUT} ({OUT.stat().st_size} bytes)")
