#!/usr/bin/env python3
"""Esquema del PWM del ventilador del frigo (P4): IRLZ44N o IRLR7843, con Rg,
pull-down, rueda libre y desacoplo. Reutiliza los simbolos de
gen_fan_wiring_pdf.py para que el estilo sea el mismo que docs/fan_wiring.pdf.

Salida: docs/esquema_pwm_ventilador.{png,pdf} (IRLZ44N) y
        docs/esquema_pwm_ventilador_7843.{png,pdf} (IRLR7843), A4 vertical.
"""
import sys
import math
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle, Circle

import gen_fan_wiring_pdf as g   # colores y simbolos ya usados en el proyecto

C_LABEL = g.C_LABEL
C_NOTE = g.C_NOTE
C_WIRE_R = g.C_WIRE_R
C_WIRE_K = g.C_WIRE_K
C_WIRE_Y = g.C_WIRE_Y
C_WIRE_G = g.C_WIRE_G

DOCS = pathlib.Path(__file__).resolve().parent.parent / "docs"
SALIDA = DOCS / "esquema_pwm_ventilador"


def resistor_v(ax, x, y1, y2, label, lado="right"):
    """Resistencia vertical entre (x,y1) y (x,y2) (y1 arriba)."""
    yc = (y1 + y2) / 2.0
    cuerpo = 0.9
    ax.add_line(Line2D([x, x], [y1, yc + cuerpo / 2], color=g.C_R_LEAD, linewidth=1.4))
    ax.add_line(Line2D([x, x], [yc - cuerpo / 2, y2], color=g.C_R_LEAD, linewidth=1.4))
    ax.add_patch(Rectangle((x - 0.17, yc - cuerpo / 2), 0.34, cuerpo,
                           facecolor=g.C_R_BODY, edgecolor="#666", linewidth=0.6))
    dx = 0.30 if lado == "right" else -0.30
    ha = "left" if lado == "right" else "right"
    ax.text(x + dx, yc, label, ha=ha, va="center", fontsize=8.5,
            color=C_LABEL, fontweight="bold")


def capacitor_v(ax, x, y1, y2, label, polarizado=False):
    """Condensador vertical entre (x,y1) y (x,y2) (y1 arriba)."""
    yc = (y1 + y2) / 2.0
    hueco = 0.10
    ancho = 0.26
    ax.add_line(Line2D([x, x], [y1, yc + hueco], color=g.C_R_LEAD, linewidth=1.4))
    ax.add_line(Line2D([x, x], [yc - hueco, y2], color=g.C_R_LEAD, linewidth=1.4))
    ax.add_line(Line2D([x - ancho, x + ancho], [yc + hueco, yc + hueco],
                       color=C_LABEL, linewidth=2.0))
    ax.add_line(Line2D([x - ancho, x + ancho], [yc - hueco, yc - hueco],
                       color=C_LABEL, linewidth=2.0))
    if polarizado:   # + junto a la placa de arriba
        ax.text(x + ancho + 0.08, yc + hueco + 0.10, "+", ha="left", va="center",
                fontsize=9, color=C_LABEL, fontweight="bold")
    ax.text(x + ancho + 0.14, yc, label, ha="left", va="center", fontsize=8.5,
            color=C_LABEL, fontweight="bold")


def gnd_symbol(ax, x, y):
    """Simbolo de masa (tres barras) colgando de (x,y)."""
    for i, w in enumerate((0.26, 0.17, 0.08)):
        yy = y - 0.12 * (i + 1)
        ax.add_line(Line2D([x - w, x + w], [yy, yy], color=C_LABEL, linewidth=1.6))


def punto(ax, x, y):
    ax.add_patch(Circle((x, y), 0.075, facecolor=C_LABEL, edgecolor=C_LABEL,
                        zorder=6))


def esquema(q1="IRLZ44N", q1_nota="TO-220 · 55 V · logic-level",
            salida=SALIDA, subtitulo="low-side con IRLZ44N"):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("PWM del ventilador del frigo (P4) — " + subtitulo,
                 fontsize=15, fontweight="bold", y=0.965)
    fig.text(0.5, 0.938,
             "GPIO 5 · LEDC 10 bits · 18 kHz · ventilador de 3 hilos a 12 V "
             "(≤ 0,5 A)",
             ha="center", fontsize=9, color=C_NOTE)

    ax = fig.add_axes([0.03, 0.33, 0.94, 0.59])
    ax.set_xlim(0, 17.2)
    ax.set_ylim(0.4, 10.6)
    ax.set_aspect("equal")
    ax.set_axis_off()

    # ── Rails ────────────────────────────────────────────────────────────────
    Y12, YGND = 9.6, 1.8
    # +12V
    g.labeled_box(ax, 0.5, Y12 - 0.4, 2.4, 0.8, "+12V\nautocaravana",
                  bg="#FFEBEE", edge=C_WIRE_R, fontsize=9)
    g.wire(ax, [(2.9, Y12), (3.7, Y12)], C_WIRE_R)
    g.fuse(ax, 4.2, Y12, label="F1 1 A")
    g.wire(ax, [(4.7, Y12), (12.4, Y12)], C_WIRE_R)
    # GND
    g.labeled_box(ax, 0.5, YGND - 0.4, 2.4, 0.8, "GND\nautocaravana",
                  bg="#EEEEEE", edge=C_WIRE_K, fontsize=9)
    g.wire(ax, [(2.9, YGND), (12.4, YGND)], C_WIRE_K)

    # ── Desacoplo junto al ventilador (100 uF + 100 nF) ─────────────────────
    XC2, XC1 = 5.5, 6.5
    for x, pol in ((XC2, True), (XC1, False)):
        ax.add_line(Line2D([x, x], [Y12, Y12 - 0.2], color=C_WIRE_R, linewidth=2.2))
        punto(ax, x, Y12)
        capacitor_v(ax, x, Y12 - 0.2, Y12 - 1.55, "", polarizado=pol)
        ax.add_line(Line2D([x, x], [Y12 - 1.55, Y12 - 1.75], color=C_WIRE_K,
                           linewidth=2.2))
    ax.text(XC2 - 0.42, Y12 - 0.95, "C2\n100 µF\n25 V", ha="right", va="center",
            fontsize=7.8, color=C_LABEL, fontweight="bold")
    ax.text(XC1 + 0.42, Y12 - 0.95, "C1\n100 nF", ha="left", va="center",
            fontsize=7.8, color=C_LABEL, fontweight="bold")
    # las dos a la misma masa, con un solo simbolo
    g.wire(ax, [(XC2, Y12 - 1.75), (XC1, Y12 - 1.75)], C_WIRE_K)
    gnd_symbol(ax, (XC2 + XC1) / 2.0, Y12 - 1.75)

    # ── Ventilador (3 pines) ─────────────────────────────────────────────────
    g.fan_symbol(ax, 15.2, 7.55, radius=1.05, n_pin=3)
    PIN_X = 12.4
    for y in (Y12, 6.3, 7.9):
        ax.add_patch(Circle((PIN_X, y), 0.17, facecolor="white",
                            edgecolor="black", linewidth=0.8, zorder=5))
    ax.text(PIN_X + 0.30, Y12, "VCC", ha="left", va="center", fontsize=9,
            color=C_LABEL, fontweight="bold")
    ax.text(PIN_X + 0.30, 6.3, "GND", ha="left", va="center", fontsize=9,
            color=C_LABEL, fontweight="bold")
    ax.text(PIN_X + 0.30, 7.9, "TACH", ha="left", va="center", fontsize=9,
            color=C_LABEL, fontweight="bold")
    ax.text(PIN_X + 0.30, 7.55, "(sin usar)", ha="left", va="top", fontsize=7.5,
            color=C_NOTE, style="italic")
    ax.add_line(Line2D([PIN_X - 0.9, PIN_X - 0.17], [7.9, 7.9], color=C_WIRE_Y,
                       linewidth=2.0, linestyle="--"))
    ax.text(PIN_X - 0.55, 7.9, "X", ha="center", va="center", fontsize=14,
            color=g.C_CUT, fontweight="bold")

    # ── Rueda libre: D1 entre la masa del fan y +12 V ────────────────────────
    XD = 9.6
    punto(ax, XD, Y12)
    g.diode(ax, XD, 6.3, XD, Y12, "", anode_first=True, label_pos="right")
    ax.text(XD, Y12 + 0.28, "D1 · 1N5819", ha="center", va="bottom", fontsize=8,
            color=C_LABEL, fontweight="bold")

    # ── MOSFET y su malla ────────────────────────────────────────────────────
    XQ = 10.2
    g.mosfet_n(ax, XQ, 3.4, label="")
    ax.text(XQ + 0.95, 3.55, f"Q1\n{q1}", ha="left", va="center", fontsize=8.5,
            color=C_LABEL, fontweight="bold")
    ax.text(XQ + 0.95, 3.0, q1_nota, ha="left", va="top", fontsize=7,
            color=C_NOTE, style="italic")
    # Drain → masa del ventilador
    g.wire(ax, [(XQ + 0.15, 3.95), (XQ + 0.15, 6.3)], C_WIRE_K)
    g.wire(ax, [(PIN_X - 0.17, 6.3), (XD, 6.3)], C_WIRE_K)
    punto(ax, XD, 6.3)
    # Source → GND
    g.wire(ax, [(XQ + 0.15, 2.85), (XQ + 0.15, YGND)], C_WIRE_K)
    punto(ax, XQ + 0.15, YGND)

    # ── Puerta: GPIO → Rg → G, con R1 a masa ─────────────────────────────────
    YG = 3.65
    g.labeled_box(ax, 0.6, YG - 0.45, 2.8, 0.9,
                  "GPIO 5\nPWM 18 kHz · 3,3 V", bg="#E8F5E9", edge=C_WIRE_G,
                  fontsize=9)
    g.wire(ax, [(3.4, YG), (5.0, YG)], C_WIRE_G)
    g.resistor(ax, 5.0, YG, 6.2, YG, "")
    ax.text(5.6, YG + 0.42, "Rg\n100 Ω", ha="center", va="bottom", fontsize=8.5,
            color=C_LABEL, fontweight="bold")
    g.wire(ax, [(6.2, YG), (XQ - 0.55, YG)], C_WIRE_G)
    # R1 pull-down
    XR = 8.6
    punto(ax, XR, YG)
    resistor_v(ax, XR, YG, YGND, "R1\n10 kΩ")
    punto(ax, XR, YGND)

    # ── Simbolo de masa comun, colgando del rail de GND ─────────────────────
    gnd_symbol(ax, 3.7, YGND)
    ax.text(3.7, YGND - 0.60, "masa común\n(en estrella)", ha="center",
            va="top", fontsize=7.5, color=C_NOTE, style="italic")

    # Lo de "cómo conmuta" va a la derecha de las notas (debajo del dibujo),
    # que dentro se cruzaba con el cable de la puerta.

    # ── Notas / tabla de componentes ─────────────────────────────────────────
    notes = fig.add_axes([0.05, 0.03, 0.90, 0.28])
    notes.set_axis_off()
    notes.text(0.0, 1.0, "Componentes", fontsize=11, fontweight="bold",
               transform=notes.transAxes, va="top")
    filas = [
        ("Q1", q1, "N-MOSFET logic-level. Masa del fan."),
        ("Rg", "100 Ω", "Serie con la puerta: pico del GPIO ≈ 33 mA."),
        ("R1", "10 kΩ", "Pull-down: fan apagado en reset/OTA."),
        ("D1", "1N5819", "Rueda libre: cátodo a +12 V, ánodo a masa."),
        ("C2", "100 µF 25 V", "Los pulsos del PWM salen de aquí."),
        ("C1", "100 nF", "Cerámico: rizado y ringing de los flancos."),
        ("F1", "1 A", "Protege el cable (0,1-0,5 A)."),
    ]
    y = 0.88
    for ref, valor, para in filas:
        notes.text(0.01, y, f"{ref:<3}", fontsize=8.5, family="DejaVu Sans Mono",
                   transform=notes.transAxes, va="center", color=C_LABEL,
                   fontweight="bold")
        notes.text(0.055, y, f"{valor:<13}", fontsize=8.5,
                   family="DejaVu Sans Mono", transform=notes.transAxes,
                   va="center", color=C_LABEL, fontweight="bold")
        notes.text(0.19, y, para, fontsize=7.4, transform=notes.transAxes,
                   va="center", color=C_LABEL)
        y -= 0.085

    # Columna derecha: como conmuta
    notes.text(0.70, 0.95, "Cómo conmuta", fontsize=10, fontweight="bold",
               transform=notes.transAxes, va="top")
    yc = 0.86
    for txt in (
        "GPIO alto → MOSFET ON → el fan gira",
        "GPIO bajo → MOSFET OFF → parado",
        "0 % = parado, 100 % = máximo",
        "Arranque: pulso al 100 % (kickstart)",
        "y luego al duty pedido",
        "Por debajo del duty mínimo no gira",
    ):
        notes.text(0.70, yc, "· " + txt, fontsize=7.6,
                   transform=notes.transAxes, va="top", color=C_LABEL)
        yc -= 0.075

    y -= 0.02
    for txt in (
        "Montaje: el par +12 V/GND del ventilador, trenzado y corto; el MOSFET pegado al conector; "
        "la masa del motor vuelve por su cable, no por la de la P4 (el ADXL y el I2C son lo que más sufre el ruido).",
        "No poner condensador entre puerta y fuente (con Rg formaría un filtro de 10 µs y el MOSFET trabajaría en la zona lineal).",
        "El TACH deja de servir: su referencia es la masa conmutada del ventilador.",
    ):
        notes.text(0.01, y, txt, fontsize=7.8, transform=notes.transAxes,
                   va="top", color=C_NOTE, wrap=True)
        y -= 0.075

    salida.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(salida) + ".png", dpi=300, facecolor="white")
    with PdfPages(str(salida) + ".pdf") as pdf:
        pdf.savefig(fig, facecolor="white")
    plt.close(fig)
    print("OK ->", salida.with_suffix(".png"), "y .pdf")


if __name__ == "__main__":
    esquema()
    # La misma hoja con el MOSFET que lleva la placa montada (DPAK).
    esquema(q1="IRLR7843", q1_nota="DPAK (TO-252) · 30 V · ~4,5 mΩ a 4,5 V",
            salida=DOCS / "esquema_pwm_ventilador_7843",
            subtitulo="low-side con IRLR7843")
