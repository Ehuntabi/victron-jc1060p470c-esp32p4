#!/usr/bin/env python3
"""Esquema teorico del bus RS-485 NE185 + bias/terminacion + sniff con NE187.

Pag 1 - Esquema general del bus con los 3 dispositivos en paralelo y las 3
        resistencias de bias/terminacion.
Pag 2 - Funcionamiento del bias (estado idle vs transmitting) y cuando
        hace falta cada resistencia.
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Rectangle, Circle, FancyBboxPatch, Polygon
from matplotlib.lines import Line2D
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs" / "rs485_bus_theory.pdf"

# Colores
C_LABEL = "#222"
C_R     = "#D32F2F"     # +5V rojo
C_K     = "#1A1A1A"     # GND negro
C_B     = "#1976D2"     # bus azul
C_5V    = "#D32F2F"
C_NOTE  = "#555"
C_R_BODY = "#D0B080"


def labeled_box(ax, x, y, w, h, text, bg, edge, fontsize=10):
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                 boxstyle="round,pad=0,rounding_size=0.15",
                                 facecolor=bg, edgecolor=edge, linewidth=1.5))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fontsize, fontweight="bold", color=C_LABEL)


def resistor_v(ax, cx, y1, y2, label, label_side="right"):
    """Resistencia vertical entre (cx, y1) abajo y (cx, y2) arriba."""
    ax.add_line(Line2D([cx, cx], [y1, y1 + 0.25], color="#333", linewidth=1.2))
    ax.add_line(Line2D([cx, cx], [y2 - 0.25, y2], color="#333", linewidth=1.2))
    body_h = (y2 - y1) - 0.5
    ax.add_patch(FancyBboxPatch((cx - 0.18, y1 + 0.25), 0.36, body_h,
                                 boxstyle="round,pad=0,rounding_size=0.06",
                                 facecolor=C_R_BODY, edgecolor="#666",
                                 linewidth=0.6))
    dx = 0.30 if label_side == "right" else -0.30
    ha = "left" if label_side == "right" else "right"
    ax.text(cx + dx, (y1 + y2) / 2, label, ha=ha, va="center",
            fontsize=9, color=C_LABEL, fontweight="bold")


def device(ax, x, y, w, h, name, sub, bg, edge):
    """Caja de dispositivo (NE185, NE187, 7\")."""
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                 boxstyle="round,pad=0,rounding_size=0.15",
                                 facecolor=bg, edgecolor=edge, linewidth=1.6))
    ax.text(x + w / 2, y + h / 2 + 0.2, name, ha="center", va="center",
            fontsize=11, fontweight="bold", color=C_LABEL)
    if sub:
        ax.text(x + w / 2, y + h / 2 - 0.3, sub, ha="center", va="center",
                fontsize=8, color=C_NOTE, style="italic")


# ════════════════════════════════════════════════════════════════
# PAGINA 1 - Esquema general del bus RS-485
# ════════════════════════════════════════════════════════════════

def page1(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Bus RS-485 NE185 - esquema teorico",
                  fontsize=15, fontweight="bold", y=0.97)
    fig.text(0.5, 0.94,
              "Hasta 32 dispositivos en multidrop (mismo cable A/B/GND). "
              "Bias + terminacion fijan el estado idle.",
              ha="center", fontsize=9, color=C_NOTE)

    ax = fig.add_axes([0.04, 0.40, 0.92, 0.52])
    ax.set_xlim(0, 20)
    ax.set_ylim(0, 11)
    ax.set_aspect("equal")
    ax.set_axis_off()

    # ── 3 dispositivos en paralelo al bus ─────────────────────
    # Centralita NE185 (izquierda)
    device(ax, 0.5, 5.0, 3.5, 2.0, "NE185", "centralita Nordelettronica",
           "#E8F5E9", "#388E3C")
    # NE187 (centro-arriba) - solo en modo sniff/test
    device(ax, 8.0, 8.0, 3.5, 2.0, "NE187", "panel original (sniff)",
           "#FFF8E1", "#F9A825")
    # 7" Guition (derecha)
    device(ax, 15.0, 5.0, 4.0, 2.0, '7" Guition',
           "ESP32-P4 + MAX485 onboard", "#E3F2FD", "#1976D2")

    # ── Bus horizontal A, B, GND (largos) ──────────────────────
    bus_a_y = 4.0
    bus_b_y = 3.0
    bus_g_y = 2.0
    # A
    ax.plot([0, 20], [bus_a_y, bus_a_y], color=C_B, linewidth=3,
            solid_capstyle="round", zorder=2)
    ax.text(0.0, bus_a_y + 0.3, "A (bus)", ha="left", va="bottom",
            fontsize=9, color=C_B, fontweight="bold")
    # B
    ax.plot([0, 20], [bus_b_y, bus_b_y], color=C_B, linewidth=3,
            solid_capstyle="round", zorder=2)
    ax.text(0.0, bus_b_y + 0.3, "B (bus)", ha="left", va="bottom",
            fontsize=9, color=C_B, fontweight="bold")
    # GND
    ax.plot([0, 20], [bus_g_y, bus_g_y], color=C_K, linewidth=3,
            solid_capstyle="round", zorder=2)
    ax.text(0.0, bus_g_y + 0.3, "GND comun", ha="left", va="bottom",
            fontsize=9, color=C_K, fontweight="bold")

    # ── Conexiones de los dispositivos al bus ──────────────────
    # NE185: 3 lineas verticales
    for x, y_dev, y_bus, color in [
        (1.5, 5.0, bus_a_y, C_B), (2.25, 5.0, bus_b_y, C_B), (3.0, 5.0, bus_g_y, C_K)
    ]:
        ax.plot([x, x], [y_bus, y_dev], color=color, linewidth=2,
                solid_capstyle="round", zorder=2)
    ax.text(1.5, 4.7, "A", ha="center", va="bottom", fontsize=7, color=C_B,
            fontweight="bold")
    ax.text(2.25, 4.7, "B", ha="center", va="bottom", fontsize=7, color=C_B,
            fontweight="bold")
    ax.text(3.0, 4.7, "G", ha="center", va="bottom", fontsize=7, color=C_K,
            fontweight="bold")

    # NE187: 3 lineas verticales bajando al bus
    for x, color, label in [(9.0, C_B, "A"), (9.75, C_B, "B"), (10.5, C_K, "G")]:
        ax.plot([x, x], [bus_a_y if label == "A" else (bus_b_y if label == "B" else bus_g_y), 8.0],
                color=color, linewidth=2, solid_capstyle="round", zorder=2)
        ax.text(x, 7.7, label, ha="center", va="bottom", fontsize=7,
                color=color, fontweight="bold")
    # Linea punteada indicando "opcional"
    ax.text(9.75, 6.0, "(solo para sniff)", ha="center", va="center",
            fontsize=8, color=C_NOTE, style="italic",
            bbox=dict(facecolor="white", edgecolor="none"))

    # 7": 3 lineas verticales
    for x, y_dev, y_bus, color, lbl in [
        (16.0, 5.0, bus_a_y, C_B, "A"),
        (16.75, 5.0, bus_b_y, C_B, "B"),
        (17.5, 5.0, bus_g_y, C_K, "G"),
    ]:
        ax.plot([x, x], [y_bus, y_dev], color=color, linewidth=2,
                solid_capstyle="round", zorder=2)
        ax.text(x, 4.7, lbl, ha="center", va="bottom", fontsize=7,
                color=color, fontweight="bold")

    # ── BIAS + TERMINACION (cerca del 7", podria ir en cualquier extremo) ──
    bias_x_base = 13.0
    # +5V rail (del 7")
    ax.plot([bias_x_base - 0.5, bias_x_base + 2.5], [10.0, 10.0],
            color=C_5V, linewidth=2.5)
    ax.text(bias_x_base + 1.0, 10.3, "+5V (del 7\")", ha="center",
            va="bottom", fontsize=9, color=C_5V, fontweight="bold")

    # R1 (680): bias pull-up A → +5V
    r1_x = bias_x_base + 0.0
    resistor_v(ax, r1_x, bus_a_y, 10.0, "R1 = 680 ohm\n(A → +5V, pull-up)")
    # R3 (680): bias pull-down B → GND
    r3_x = bias_x_base + 2.5
    resistor_v(ax, r3_x, bus_g_y, bus_b_y, "R3 = 680 ohm\n(B → GND, pull-down)",
               label_side="left")
    # R2 (120): terminacion A ↔ B
    r2_x = bias_x_base + 1.25
    resistor_v(ax, r2_x, bus_b_y, bus_a_y, "R2 = 120 ohm\n(A ↔ B, terminacion)")

    # Recuadro destacando "AÑADIR ESTAS 3"
    ax.add_patch(Rectangle((bias_x_base - 0.8, 1.6), 4.1, 9.0,
                            fill=False, edgecolor=C_R, linewidth=2.0,
                            linestyle="--"))
    ax.text(bias_x_base + 1.25, 10.6, "AÑADIR ESTAS 3 RESISTENCIAS",
            ha="center", va="bottom", fontsize=9, color=C_R, fontweight="bold")

    # ── Notas ──────────────────────────────────────────────────
    notes = fig.add_axes([0.05, 0.04, 0.90, 0.32])
    notes.set_axis_off()
    notes.text(0.0, 1.0, "Quien va donde:",
               fontsize=11, fontweight="bold",
               transform=notes.transAxes, va="top")
    items = [
        ("NE185 (verde) = centralita Nordelettronica",
         "alimentada en su entrada propia 12V de la autocaravana."),
        ("NE187 (amarillo) = panel original con LEDs y boton check",
         "se conecta para hacer sniff; despues queda fuera (lo retiraste)."),
        ("7\" Guition (azul) = ESP32-P4 + MAX485 onboard via J5",
         "extremo del bus en el otro lado."),
        ("R1, R2, R3 = bias + terminacion",
         "se sueldan en el cable del 7\". El NE187 tenia estas dentro y al "
         "quitarlo perdimos el bias del bus."),
    ]
    y = 0.92
    for name, desc in items:
        notes.text(0.02, y, "● ", fontsize=10,
                   transform=notes.transAxes, color=C_LABEL)
        notes.text(0.04, y, name, fontsize=9, fontweight="bold",
                   transform=notes.transAxes, color=C_LABEL)
        notes.text(0.04, y - 0.04, desc, fontsize=8.5,
                   transform=notes.transAxes, color=C_NOTE)
        y -= 0.11

    pdf.savefig(fig)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════
# PAGINA 2 - Por que el bias es necesario
# ════════════════════════════════════════════════════════════════

def page2(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Por que el bias y la terminacion son necesarios",
                  fontsize=15, fontweight="bold", y=0.97)
    fig.text(0.5, 0.94,
              "En RS-485 'idle' el bus debe tener un estado electrico bien definido. "
              "Sin bias, el bus flota y el receptor lee ruido como bits aleatorios.",
              ha="center", fontsize=9, color=C_NOTE)

    # ── SUB ESQUEMA 1: SIN bias (mal) ───────────────────────────
    ax1 = fig.add_axes([0.04, 0.55, 0.92, 0.36])
    ax1.set_xlim(0, 16)
    ax1.set_ylim(0, 6)
    ax1.set_aspect("equal")
    ax1.set_axis_off()
    ax1.text(8, 5.5, "SIN bias (el bus flota cuando nadie transmite)",
             ha="center", va="bottom", fontsize=11, fontweight="bold",
             color=C_R)

    # Cables A, B flotando
    ax1.plot([1, 15], [3.5, 3.5], color=C_B, linewidth=2.5, linestyle="--")
    ax1.text(0.5, 3.5, "A", ha="right", va="center", fontsize=10,
             color=C_B, fontweight="bold")
    ax1.plot([1, 15], [2.5, 2.5], color=C_B, linewidth=2.5, linestyle="--")
    ax1.text(0.5, 2.5, "B", ha="right", va="center", fontsize=10,
             color=C_B, fontweight="bold")
    # Ruido visual
    for x in [3, 5, 7, 9, 11, 13]:
        ax1.text(x, 3.5, "?", ha="center", va="center", fontsize=14,
                 color=C_R, fontweight="bold")
        ax1.text(x, 2.5, "?", ha="center", va="center", fontsize=14,
                 color=C_R, fontweight="bold")
    ax1.text(8, 1.5,
             "El receptor lee BASURA (bits aleatorios) -> tramas corruptas, "
             "no se valida ni se sincroniza.",
             ha="center", va="center", fontsize=9, color=C_R,
             fontweight="bold", wrap=True)
    ax1.text(8, 0.6,
             "Es lo que tenemos AHORA en el 7\" tras quitar el NE187.",
             ha="center", va="center", fontsize=9, color=C_R, style="italic")

    # ── SUB ESQUEMA 2: CON bias (bien) ───────────────────────────
    ax2 = fig.add_axes([0.04, 0.10, 0.92, 0.40])
    ax2.set_xlim(0, 16)
    ax2.set_ylim(0, 7)
    ax2.set_aspect("equal")
    ax2.set_axis_off()
    ax2.text(8, 6.5, "CON bias + terminacion (idle bien definido)",
             ha="center", va="bottom", fontsize=11, fontweight="bold",
             color="#388E3C")

    # +5V
    ax2.plot([4, 12], [5.5, 5.5], color=C_5V, linewidth=2.5)
    ax2.text(8, 5.8, "+5V", ha="center", va="bottom", fontsize=9,
             color=C_5V, fontweight="bold")

    # Cables A, B (firmes, sin lineas punteadas)
    ax2.plot([1, 15], [4.0, 4.0], color=C_B, linewidth=2.5)
    ax2.text(0.5, 4.0, "A", ha="right", va="center", fontsize=10,
             color=C_B, fontweight="bold")
    ax2.plot([1, 15], [3.0, 3.0], color=C_B, linewidth=2.5)
    ax2.text(0.5, 3.0, "B", ha="right", va="center", fontsize=10,
             color=C_B, fontweight="bold")
    # GND
    ax2.plot([4, 12], [1.5, 1.5], color=C_K, linewidth=2.5)
    ax2.text(8, 1.2, "GND", ha="center", va="top", fontsize=9,
             color=C_K, fontweight="bold")

    # R1 (680 pull-up A → +5V)
    resistor_v(ax2, 5.5, 4.0, 5.5, "R1\n680 ohm")
    # R3 (680 pull-down B → GND)
    resistor_v(ax2, 10.5, 1.5, 3.0, "R3\n680 ohm", label_side="left")
    # R2 (120 terminacion A ↔ B)
    resistor_v(ax2, 8.0, 3.0, 4.0, "R2\n120 ohm")

    # Estado del bus en idle:
    ax2.text(13.5, 4.0, "HIGH", ha="left", va="center", fontsize=11,
             color="#388E3C", fontweight="bold")
    ax2.text(13.5, 3.0, "LOW", ha="left", va="center", fontsize=11,
             color="#388E3C", fontweight="bold")
    ax2.annotate("", xy=(13.4, 4.0), xytext=(15.0, 4.0),
                 arrowprops=dict(arrowstyle="<-", color="#388E3C", lw=1.5))
    ax2.annotate("", xy=(13.4, 3.0), xytext=(15.0, 3.0),
                 arrowprops=dict(arrowstyle="<-", color="#388E3C", lw=1.5))

    # Notas finales
    notes_y = 0.45
    fig.text(0.05, notes_y, "Funcion:", fontsize=11, fontweight="bold",
             color=C_LABEL)
    items = [
        "R1 (680 ohm A → +5V): TIRA del cable A hacia HIGH (3.3-5V) durante idle.",
        "R3 (680 ohm B → GND): TIRA del cable B hacia LOW (0V) durante idle.",
        "Con A=HIGH y B=LOW -> diferencia A-B positiva -> receptor lee BIT 1 (idle line).",
        "Cuando alguien transmite, sus drivers vencen al bias (mas potentes) e imponen los bits.",
        "",
        "R2 (120 ohm A ↔ B): TERMINACION = impedancia caracteristica del par trenzado.",
        "Sin ella, las seniales rebotan en los extremos del cable (ondas estacionarias).",
        "Para cables <5m no es estrictamente necesaria, pero no estorba.",
    ]
    y = notes_y - 0.04
    for txt in items:
        fig.text(0.07, y, txt, fontsize=9, color=C_LABEL)
        y -= 0.025

    pdf.savefig(fig)
    plt.close(fig)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with PdfPages(OUT) as pdf:
        page1(pdf)
        page2(pdf)
    print(f"OK -> {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
