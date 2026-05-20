#!/usr/bin/env python3
"""Genera docs/fan_wiring.pdf — cableado del ventilador del frigo (GPIO 5 PWM).

Pag 1: Esquema electrico clasico, ventilador 4-pin (directo, sin componentes).
Pag 2: Esquema electrico clasico, ventilador 3-pin (con MOSFET IRLZ44N + R10k + diodo).
Pag 3: Stripboard 12 x 16 con el cableado practico de la version 3-pin.

Backend matplotlib + PdfPages, A4 estandar (sin bbox_inches='tight'; ver
memoria feedback-matplotlib-pdf-bbox-tight). Compatible con xreader.
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Rectangle, Circle, FancyBboxPatch, Polygon
from matplotlib.lines import Line2D
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs" / "fan_wiring.pdf"

# ── Colores ─────────────────────────────────────────────────────
C_BG       = "#FFFFFF"
C_BG_FRAME = "#FAFAFA"
C_OUTLINE  = "#888888"
C_LABEL    = "#222222"
C_BOX_BG   = "#E8EEF7"
C_BOX_LINE = "#3F51B5"
C_WIRE_R   = "#D32F2F"     # rojo +12V / VCC
C_WIRE_K   = "#1A1A1A"     # negro GND
C_WIRE_Y   = "#FBC02D"     # amarillo TACH (NC)
C_WIRE_G   = "#388E3C"     # verde / azul PWM o senal
C_WIRE_B   = "#1976D2"
C_R_BODY   = "#B6BBC2"
C_R_LEAD   = "#2E2E2E"
C_STRIP    = "#FFD8A8"     # cobre stripboard
C_HOLE     = "#FFFFFF"
C_FR4      = "#FFF9F2"
C_CUT      = "#D32F2F"
C_FAN_BODY = "#37474F"
C_NOTE     = "#555555"


# ════════════════════════════════════════════════════════════════
# Helpers comunes - esquema electrico
# ════════════════════════════════════════════════════════════════

def labeled_box(ax, x, y, w, h, text, bg=C_BOX_BG, edge=C_BOX_LINE, fontsize=10):
    """Caja rectangular con texto centrado (componente / fuente)."""
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                 boxstyle="round,pad=0.0,rounding_size=0.10",
                                 facecolor=bg, edgecolor=edge, linewidth=1.4))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fontsize, color=C_LABEL, fontweight="bold")


def connector(ax, x, y, n_pins, labels, colors, vertical=True, dotsize=0.18):
    """Conector con n_pines, etiquetas y color de cable por pin."""
    if vertical:
        for i in range(n_pins):
            cx, cy = x, y - i * 0.5
            ax.add_patch(Circle((cx, cy), dotsize,
                                 facecolor=colors[i], edgecolor="black",
                                 linewidth=0.6, zorder=5))
            ax.text(cx - 0.45, cy, labels[i], ha="right", va="center",
                    fontsize=8.5, color=C_LABEL)
    else:
        for i in range(n_pins):
            cx, cy = x + i * 0.5, y
            ax.add_patch(Circle((cx, cy), dotsize,
                                 facecolor=colors[i], edgecolor="black",
                                 linewidth=0.6, zorder=5))
            ax.text(cx, cy + 0.4, labels[i], ha="center", va="bottom",
                    fontsize=8.5, color=C_LABEL)


def wire(ax, pts, color, lw=2.2):
    """Cable con segmentos. pts = [(x1,y1),(x2,y2),...]."""
    xs, ys = zip(*pts)
    ax.plot(xs, ys, color=color, linewidth=lw,
            solid_capstyle="round", solid_joinstyle="miter", zorder=2)


def mosfet_n(ax, cx, cy, label="IRLZ44N"):
    """MOSFET N-channel logic-level con G (izda), D (arriba), S (abajo)."""
    # Cuerpo
    ax.add_patch(Circle((cx, cy), 0.55, facecolor="white",
                         edgecolor=C_LABEL, linewidth=1.4))
    # Linea vertical interna (canal)
    ax.add_line(Line2D([cx + 0.15, cx + 0.15], [cy - 0.35, cy + 0.35],
                       color=C_LABEL, linewidth=1.6))
    # Lineas G, D, S (interno)
    ax.add_line(Line2D([cx - 0.30, cx + 0.05], [cy + 0.25, cy + 0.25],
                       color=C_LABEL, linewidth=1.2))   # Gate
    ax.add_line(Line2D([cx - 0.30, cx + 0.05], [cy, cy],
                       color=C_LABEL, linewidth=1.2))   # canal medio
    ax.add_line(Line2D([cx - 0.30, cx + 0.05], [cy - 0.25, cy - 0.25],
                       color=C_LABEL, linewidth=1.2))
    # Gate (linea separada)
    ax.add_line(Line2D([cx - 0.30, cx - 0.30], [cy - 0.25, cy + 0.25],
                       color=C_LABEL, linewidth=1.4))
    # Flecha N-channel
    ax.add_patch(Polygon([[cx + 0.05, cy], [cx - 0.05, cy + 0.08],
                          [cx - 0.05, cy - 0.08]],
                         facecolor=C_LABEL, edgecolor=C_LABEL))
    # Terminales G (izda), D (arriba), S (abajo)
    ax.add_line(Line2D([cx - 0.55, cx - 0.30], [cy + 0.25, cy + 0.25],
                       color=C_LABEL, linewidth=1.4))    # Gate out
    ax.add_line(Line2D([cx + 0.15, cx + 0.15], [cy + 0.55, cy + 0.25],
                       color=C_LABEL, linewidth=1.4))    # Drain
    ax.add_line(Line2D([cx + 0.15, cx + 0.15], [cy - 0.55, cy - 0.25],
                       color=C_LABEL, linewidth=1.4))    # Source
    # Etiquetas
    ax.text(cx - 0.60, cy + 0.25, "G", ha="right", va="center", fontsize=8,
            color=C_LABEL, fontweight="bold")
    ax.text(cx + 0.20, cy + 0.65, "D", ha="left", va="bottom", fontsize=8,
            color=C_LABEL, fontweight="bold")
    ax.text(cx + 0.20, cy - 0.65, "S", ha="left", va="top", fontsize=8,
            color=C_LABEL, fontweight="bold")
    ax.text(cx, cy - 1.0, label, ha="center", va="top",
            fontsize=9, color=C_LABEL, fontweight="bold")


def resistor(ax, x1, y1, x2, y2, label):
    """Resistencia entre dos puntos (zigzag horizontal) con etiqueta."""
    # Patillas
    ax.add_line(Line2D([x1, x1 + 0.3], [y1, y1], color=C_R_LEAD, linewidth=1.4))
    ax.add_line(Line2D([x2 - 0.3, x2], [y2, y2], color=C_R_LEAD, linewidth=1.4))
    # Cuerpo
    body_x = x1 + 0.3
    body_w = (x2 - x1) - 0.6
    ax.add_patch(FancyBboxPatch((body_x, y1 - 0.13), body_w, 0.26,
                                 boxstyle="round,pad=0,rounding_size=0.05",
                                 facecolor=C_R_BODY, edgecolor="#666",
                                 linewidth=0.6))
    ax.text((x1 + x2) / 2, y1 + 0.35, label, ha="center", va="bottom",
            fontsize=8.5, color=C_LABEL, fontweight="bold")


def diode(ax, x1, y1, x2, y2, label, anode_first=True, label_pos="right"):
    """Diodo entre 2 puntos.
    anode_first=True: ánodo en (x1,y1), cátodo en (x2,y2). Sentido conduccion x1→x2.
    label_pos: 'right' (a la derecha del simbolo) o 'top'."""
    mid = ((x1 + x2) / 2, (y1 + y2) / 2)
    vertical = abs(x2 - x1) < 0.01
    if vertical:
        # Diodo vertical: dibujar triangulo apuntando hacia y2
        upward = y2 > y1
        tri_size = 0.2
        tip_y = mid[1] + (tri_size if upward else -tri_size)
        base_y = mid[1] - (tri_size if upward else -tri_size)
        # Patillas
        ax.add_line(Line2D([x1, x1], [y1, base_y], color=C_R_LEAD, linewidth=1.4))
        ax.add_line(Line2D([x2, x2], [tip_y, y2], color=C_R_LEAD, linewidth=1.4))
        if anode_first:
            # Anodo en (x1,y1) -> conduce hacia y2 catodo
            tri = [[mid[0] - tri_size, base_y], [mid[0] + tri_size, base_y],
                   [mid[0], tip_y]]
        else:
            tri = [[mid[0] - tri_size, tip_y], [mid[0] + tri_size, tip_y],
                   [mid[0], base_y]]
        ax.add_patch(Polygon(tri, facecolor=C_LABEL, edgecolor=C_LABEL))
        # Linea catodo (horizontal en la punta del triangulo)
        ax.add_line(Line2D([mid[0] - tri_size - 0.05, mid[0] + tri_size + 0.05],
                           [tip_y, tip_y],
                           color=C_LABEL, linewidth=1.8))
    else:
        # Horizontal (original)
        ax.add_line(Line2D([x1, mid[0] - 0.2], [y1, mid[1]], color=C_R_LEAD, linewidth=1.4))
        ax.add_line(Line2D([mid[0] + 0.05, x2], [mid[1], y2], color=C_R_LEAD, linewidth=1.4))
        if anode_first:
            tri = [[mid[0] - 0.2, mid[1] - 0.2], [mid[0] - 0.2, mid[1] + 0.2],
                   [mid[0] + 0.05, mid[1]]]
        else:
            tri = [[mid[0] + 0.05, mid[1] - 0.2], [mid[0] + 0.05, mid[1] + 0.2],
                   [mid[0] - 0.2, mid[1]]]
        ax.add_patch(Polygon(tri, facecolor=C_LABEL, edgecolor=C_LABEL))
        cathode_x = mid[0] + 0.05 if anode_first else mid[0] - 0.2
        ax.add_line(Line2D([cathode_x, cathode_x],
                           [mid[1] - 0.2, mid[1] + 0.2],
                           color=C_LABEL, linewidth=1.6))
    if label_pos == "top":
        ax.text(mid[0], mid[1] + 0.5, label, ha="center", va="bottom",
                fontsize=8.5, color=C_LABEL, fontweight="bold")
    else:  # right
        ax.text(mid[0] + 0.5, mid[1], label, ha="left", va="center",
                fontsize=8.5, color=C_LABEL, fontweight="bold")


def fuse(ax, cx, cy, label="F 1A"):
    """Fusible (rectangulo horizontal con linea interna)."""
    ax.add_patch(Rectangle((cx - 0.5, cy - 0.18), 1.0, 0.36,
                            facecolor="white", edgecolor=C_LABEL,
                            linewidth=1.2))
    ax.add_line(Line2D([cx - 0.5, cx + 0.5], [cy, cy],
                       color=C_LABEL, linewidth=1.4))
    ax.text(cx, cy + 0.4, label, ha="center", va="bottom",
            fontsize=8.5, color=C_LABEL, fontweight="bold")


def fan_symbol(ax, cx, cy, radius=0.8, n_pin=4, label="VENTILADOR"):
    """Ventilador como circulo con aspas + N pines en la izda."""
    # Cuerpo
    ax.add_patch(Circle((cx, cy), radius, facecolor=C_FAN_BODY,
                         edgecolor=C_LABEL, linewidth=1.4, zorder=2))
    # Aspas (3 lineas a 120 grados)
    import math
    for k in range(3):
        ang = math.radians(30 + 120 * k)
        ax.add_line(Line2D([cx, cx + radius * 0.7 * math.cos(ang)],
                           [cy, cy + radius * 0.7 * math.sin(ang)],
                           color="white", linewidth=2.2, zorder=3))
    # Etiqueta
    ax.text(cx, cy - radius - 0.3, f"{label}\n({n_pin} pin)",
            ha="center", va="top", fontsize=9, color=C_LABEL, fontweight="bold")


# ════════════════════════════════════════════════════════════════
# PAGINA 1 - Esquema ventilador 4-pin (directo)
# ════════════════════════════════════════════════════════════════

def page_4pin(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Ventilador 4-pin (PWM directo) — sin componentes extra",
                  fontsize=15, fontweight="bold", y=0.96)
    fig.text(0.5, 0.93,
              "El ESP32-P4 ya genera la senial PWM 25 kHz en GPIO 5; "
              "el ventilador 4-pin la acepta directamente.",
              ha="center", fontsize=9, color=C_NOTE)

    ax = fig.add_axes([0.04, 0.40, 0.92, 0.50])
    ax.set_xlim(0, 18)
    ax.set_ylim(0, 8)
    ax.set_aspect("equal")
    ax.set_axis_off()

    # Fuente +12V (izquierda arriba)
    labeled_box(ax, 0.5, 6.5, 2.2, 0.8, "+12V\nautocaravana",
                bg="#FFEBEE", edge=C_WIRE_R, fontsize=9)
    fuse(ax, 4.5, 6.9, label="F 1A")
    # +12V → VCC fan
    wire(ax, [(2.7, 6.9), (4.0, 6.9)], C_WIRE_R)
    wire(ax, [(5.0, 6.9), (12.5, 6.9)], C_WIRE_R)

    # GND (izquierda abajo)
    labeled_box(ax, 0.5, 1.5, 2.2, 0.8, "GND\n(autocarav.)",
                bg="#EEEEEE", edge=C_WIRE_K, fontsize=9)
    wire(ax, [(2.7, 1.9), (12.5, 1.9)], C_WIRE_K)

    # GPIO 5 PWM (medio izquierda)
    labeled_box(ax, 0.5, 4.7, 2.6, 0.8,
                "GPIO 5 PWM\n(JP1 pin 15)", bg="#E8F5E9", edge=C_WIRE_G,
                fontsize=9)
    wire(ax, [(3.1, 5.1), (12.5, 5.1)], C_WIRE_G)

    # Ventilador a la derecha (mas separado del area de pines/etiquetas)
    fan_symbol(ax, 16.5, 4.5, radius=1.1, n_pin=4)
    # Pines del fan (4 pines verticales) en (12.5, y); etiqueta encima del pin
    for i, (lbl, color, y) in enumerate([
        ("VCC",       C_WIRE_R, 6.9),
        ("GND",       C_WIRE_K, 1.9),
        ("PWM",       C_WIRE_G, 5.1),
        ("TACH (NC)", C_WIRE_Y, 3.5),
    ]):
        ax.add_patch(Circle((12.5, y), 0.18, facecolor=color,
                             edgecolor="black", linewidth=0.5, zorder=5))
        ax.text(13.0, y, lbl, ha="left", va="center", fontsize=9,
                color=C_LABEL, fontweight="bold")

    # TACH NC (linea cortada con X)
    ax.add_line(Line2D([10.5, 12.5], [3.5, 3.5], color=C_WIRE_Y,
                       linewidth=2.2, linestyle="--"))
    ax.text(11.5, 3.85, "no conectado", ha="center", va="bottom",
            fontsize=8, color=C_NOTE, style="italic")
    ax.text(11.5, 3.5, "X", ha="center", va="center",
            fontsize=18, color=C_CUT, fontweight="bold")

    # Notas
    notes_ax = fig.add_axes([0.05, 0.04, 0.90, 0.32])
    notes_ax.set_axis_off()
    notes_ax.text(0.0, 1.0, "Conexion (3 cables):",
                  fontsize=11, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    rows = [
        ("Rojo",     "+12V autocaravana (con fusible 1A)", "VCC fan"),
        ("Negro",    "GND comun (autocaravana = 7\")",     "GND fan"),
        ("Verde",    "GPIO 5 (JP1 pin 15) PWM 25 kHz 3.3V", "PWM fan"),
        ("Amarillo", "(no se usa)",                         "TACH fan"),
    ]
    y = 0.88
    for col, src, dst in rows:
        notes_ax.text(0.02, y, f"●  {col}: {src}  →  {dst}",
                       fontsize=9.5,
                       transform=notes_ax.transAxes, va="center",
                       color=C_LABEL)
        y -= 0.09

    notes_ax.text(0.0, y - 0.04, "Notas:", fontsize=11, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    y -= 0.13
    for txt in [
        "1. Ventilador 4-pin clasico de PC (Noctua, Arctic, etc.). El pin "
        "PWM es ENTRADA: el fan acepta senial PWM 3.3V o 5V a 25 kHz.",
        "2. GND comun OBLIGATORIO entre la masa de la autocaravana y la del "
        "7\" (sin esto la senial PWM oscila respecto a una referencia distinta y "
        "el fan responde erratico).",
        "3. El pin TACH del ventilador (salida del tacometro) se deja sin "
        "conectar si no quieres leer las RPM.",
        "4. Fusible 1A en la linea +12V: los fans de PC tipicamente "
        "consumen 0.1-0.5 A; un fusible mas grande no protege el cable.",
    ]:
        notes_ax.text(0.02, y, txt, fontsize=9,
                       transform=notes_ax.transAxes, va="top", color=C_LABEL,
                       wrap=True)
        y -= 0.11

    pdf.savefig(fig)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════
# PAGINA 2 - Esquema ventilador 3-pin (con IRLZ44N + R10k + diodo)
# ════════════════════════════════════════════════════════════════

def page_3pin(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Ventilador 3-pin (low-side MOSFET) — con IRLZ44N",
                  fontsize=15, fontweight="bold", y=0.96)
    fig.text(0.5, 0.93,
              "El ventilador 3-pin no acepta PWM directo; troceamos su GND "
              "con un MOSFET N-channel logic-level controlado por GPIO 5.",
              ha="center", fontsize=9, color=C_NOTE)

    ax = fig.add_axes([0.03, 0.36, 0.94, 0.55])
    ax.set_xlim(0, 18)
    ax.set_ylim(0, 9)
    ax.set_aspect("equal")
    ax.set_axis_off()

    # +12V con fusible (izquierda arriba)
    labeled_box(ax, 0.3, 7.4, 2.4, 0.8, "+12V\nautocaravana",
                bg="#FFEBEE", edge=C_WIRE_R, fontsize=9)
    fuse(ax, 4.8, 7.8, label="F 1A")
    # +12V → VCC fan
    wire(ax, [(2.7, 7.8), (4.3, 7.8)], C_WIRE_R)
    wire(ax, [(5.3, 7.8), (13.0, 7.8)], C_WIRE_R)

    # Ventilador (derecha arriba) - mas alejado para que no tape etiquetas
    fan_symbol(ax, 16.5, 6.4, radius=1.1, n_pin=3)
    # Pines del fan (3): VCC, GND, TACH(NC) en (13.0, y); etiquetas compactas
    fan_pins = [
        ("VCC",       C_WIRE_R, 7.8),
        ("GND",       C_WIRE_K, 5.4),
        ("TACH (NC)", C_WIRE_Y, 6.6),
    ]
    for lbl, color, y in fan_pins:
        ax.add_patch(Circle((13.0, y), 0.18, facecolor=color,
                             edgecolor="black", linewidth=0.5, zorder=5))
        ax.text(13.2, y, lbl, ha="left", va="center", fontsize=9,
                color=C_LABEL, fontweight="bold")

    # GND del fan → Drain del MOSFET
    wire(ax, [(13.0, 5.4), (8.5, 5.4), (8.5, 4.5)], C_WIRE_K)
    # MOSFET en el centro - etiqueta corta
    mosfet_n(ax, 8.5, 3.5, label="IRLZ44N")
    # Drain (arriba, ya conectado) – Source (abajo)
    wire(ax, [(8.65, 2.95), (8.65, 1.8), (0.5, 1.8)], C_WIRE_K)

    # GND label
    labeled_box(ax, 0.3, 1.4, 2.4, 0.8, "GND\n(autocarav.)",
                bg="#EEEEEE", edge=C_WIRE_K, fontsize=9)

    # Gate del MOSFET ← GPIO 5
    labeled_box(ax, 0.3, 3.4, 2.6, 0.8,
                "GPIO 5 PWM\n(JP1 pin 15)", bg="#E8F5E9", edge=C_WIRE_G,
                fontsize=9)
    wire(ax, [(2.9, 3.75), (7.95, 3.75)], C_WIRE_G)
    # Pull-down 10K de Gate a GND
    resistor(ax, 4.6, 2.5, 6.4, 2.5, "R1 = 10 kohm")
    wire(ax, [(4.6, 2.5), (4.6, 3.75)], C_WIRE_G)
    wire(ax, [(6.4, 2.5), (6.4, 1.8)], C_WIRE_K)

    # Diodo flyback en paralelo al ventilador. Lo movemos a col 11 para que
    # quede ENTRE el MOSFET y el fan, y su etiqueta a la derecha no choque.
    diode(ax, 11.0, 5.4, 11.0, 7.8, "D1\n1N5819",
          anode_first=True, label_pos="right")
    # Conectar diodo a la red: anodo (11.0, 5.4) ↔ GND-fan rail (8.5, 5.4)
    #                          catodo (11.0, 7.8) ↔ +12V rail
    wire(ax, [(8.5, 5.4), (11.0, 5.4)], C_WIRE_K)
    wire(ax, [(11.0, 7.8), (13.0, 7.8)], C_WIRE_R)

    # Notas
    notes_ax = fig.add_axes([0.05, 0.04, 0.90, 0.30])
    notes_ax.set_axis_off()
    notes_ax.text(0.0, 1.0, "Conexion (5 cables al modulo + 3 al fan):",
                  fontsize=11, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    rows = [
        ("Rojo",     "+12V autocaravana (con fusible 1A)",  "VCC fan + catodo D1"),
        ("Negro",    "GND comun → Source MOSFET, R1 pull-down", "GND general"),
        ("Verde",    "GPIO 5 (JP1 pin 15) → Gate MOSFET",    "(con R1 10k pull-down)"),
        ("Negro",    "Drain MOSFET → GND del fan + anodo D1", "GND fan"),
        ("Amarillo", "TACH del fan",                          "no conectado"),
    ]
    y = 0.93
    for col, src, dst in rows:
        notes_ax.text(0.02, y, f"●  {col}: {src}  →  {dst}",
                       fontsize=9, transform=notes_ax.transAxes, va="center",
                       color=C_LABEL)
        y -= 0.08

    notes_ax.text(0.0, y - 0.03, "Componentes:",
                  fontsize=11, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    y -= 0.12
    comp = [
        "IRLZ44N (N-MOSFET, TO-220, Vgs(th) <= 2V — acepta 3.3V del GPIO).",
        "R1 = 10 kΩ entre Gate y GND (pull-down: con GPIO flotante, fan apagado).",
        "D1 = 1N5819 Schottky (flyback): catodo a +12V, anodo a GND del fan.",
        "       (alternativas: 1N4007 si no hay Schottky; cualquier diodo >100mA).",
    ]
    for txt in comp:
        notes_ax.text(0.02, y, txt, fontsize=9,
                       transform=notes_ax.transAxes, va="top", color=C_LABEL)
        y -= 0.08

    pdf.savefig(fig)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════
# PAGINA 3 - Stripboard practico version 3-pin
# ════════════════════════════════════════════════════════════════

def draw_stripboard(ax, rows, cols, with_labels=True):
    row_labels = [chr(ord("A") + i) for i in range(rows)]
    ax.add_patch(Rectangle((-0.7, -0.7), cols - 1 + 1.4, rows - 1 + 1.4,
                            facecolor=C_FR4, edgecolor=C_OUTLINE, linewidth=1.2))
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
    ax.set_xlim(-6.0, cols - 1 + 2.0)
    ax.set_ylim(rows - 1 + 2.0, -2.0)
    ax.set_aspect("equal")
    ax.set_axis_off()


def strip_pad(ax, r, c, label, color, anchor="left"):
    ax.add_patch(Circle((c, r), 0.28, facecolor=color, edgecolor="black",
                        linewidth=0.6, zorder=7))
    dx = -0.7 if anchor == "left" else 0.7
    ha = "right" if anchor == "left" else "left"
    ax.text(c + dx, r, label, ha=ha, va="center", fontsize=7.5,
            color=C_LABEL, fontweight="bold", zorder=10,
            bbox=dict(facecolor="white", edgecolor=color, pad=2,
                       boxstyle="round,pad=0.2"))


def strip_wire(ax, r, c0, c1, color):
    ax.add_line(Line2D([c0, c1], [r, r], color=color, linewidth=2.4,
                       solid_capstyle="round", zorder=4))


def strip_v_resistor(ax, r_top, r_bot, c, label, label_pos="right"):
    ax.add_line(Line2D([c, c], [r_top + 0.25, r_bot - 0.25],
                       color=C_R_LEAD, linewidth=1.2, zorder=5))
    body_h = (r_bot - r_top) - 0.5
    ax.add_patch(FancyBboxPatch((c - 0.18, r_top + 0.25), 0.36, body_h,
                                 boxstyle="round,pad=0,rounding_size=0.06",
                                 facecolor=C_R_BODY, edgecolor="#666",
                                 linewidth=0.6, zorder=6))
    ax.add_patch(Circle((c, r_top), 0.10, facecolor=C_R_LEAD, zorder=7))
    ax.add_patch(Circle((c, r_bot), 0.10, facecolor=C_R_LEAD, zorder=7))
    if label_pos == "top":
        ax.text(c, r_top - 0.45, label, ha="center", va="center",
                fontsize=8, color=C_LABEL, fontweight="bold", zorder=7)
    else:
        ax.text(c + 0.55, (r_top + r_bot) / 2, label, ha="left", va="center",
                fontsize=8.5, color=C_LABEL, fontweight="bold", zorder=7)


def strip_v_diode(ax, r_top, r_bot, c, label):
    """Diodo vertical: catodo arriba (rayita), anodo abajo."""
    ax.add_line(Line2D([c, c], [r_top + 0.20, r_bot - 0.20],
                       color=C_R_LEAD, linewidth=1.2, zorder=5))
    # Triangulo (anodo abajo → catodo arriba)
    body_mid_y = (r_top + r_bot) / 2
    ax.add_patch(Polygon([[c - 0.15, body_mid_y + 0.15],
                          [c + 0.15, body_mid_y + 0.15],
                          [c, body_mid_y - 0.15]],
                         facecolor=C_LABEL, edgecolor=C_LABEL, zorder=6))
    ax.add_line(Line2D([c - 0.18, c + 0.18],
                       [body_mid_y + 0.15, body_mid_y + 0.15],
                       color=C_LABEL, linewidth=1.8, zorder=7))
    ax.add_patch(Circle((c, r_top), 0.10, facecolor=C_R_LEAD, zorder=7))
    ax.add_patch(Circle((c, r_bot), 0.10, facecolor=C_R_LEAD, zorder=7))
    ax.text(c + 0.55, body_mid_y, label, ha="left", va="center",
            fontsize=8.5, color=C_LABEL, fontweight="bold", zorder=7)
    # Marca '+12V (K)' arriba y 'GND fan (A)' abajo (mini etiquetas)
    ax.text(c, r_top - 0.40, "K", ha="center", va="center",
            fontsize=7, color=C_NOTE, style="italic")
    ax.text(c, r_bot + 0.40, "A", ha="center", va="center",
            fontsize=7, color=C_NOTE, style="italic")


def strip_cut(ax, r, c_left):
    """Marca de corte de pista: X roja entre el agujero c_left y c_left+1
    en la fila r. Aisla electricamente los 2 lados."""
    ax.add_line(Line2D([c_left + 0.30, c_left + 0.70],
                       [r - 0.20, r + 0.20], color=C_CUT,
                       linewidth=2.6, zorder=9))
    ax.add_line(Line2D([c_left + 0.30, c_left + 0.70],
                       [r + 0.20, r - 0.20], color=C_CUT,
                       linewidth=2.6, zorder=9))


def strip_mosfet_to220_rot90(ax, r_g, r_d, r_s, c_pins, c_body=None):
    """MOSFET TO-220 rotado 90 grados: cuerpo VERTICAL a la derecha, las 3
    patillas dobladas 90 grados entran horizontalmente en columnas c_pins,
    cada una en su propia FILA (r_g, r_d, r_s). Asi cada pin queda en su
    propia tira de cobre sin necesidad de cortes."""
    if c_body is None:
        c_body = c_pins + 1.2
    body_w = 1.3
    body_top = min(r_g, r_d, r_s) - 0.4
    body_bot = max(r_g, r_d, r_s) + 0.4
    # Cuerpo (rectangulo vertical)
    ax.add_patch(FancyBboxPatch((c_body, body_top), body_w, body_bot - body_top,
                                 boxstyle="round,pad=0,rounding_size=0.10",
                                 facecolor="#2A2A2A", edgecolor=C_LABEL,
                                 linewidth=1.0, zorder=6))
    # 3 patillas horizontales: de c_pins (agujero) a c_body (cuerpo)
    for rr, lbl in [(r_g, "G"), (r_d, "D"), (r_s, "S")]:
        ax.add_line(Line2D([c_pins, c_body], [rr, rr],
                           color=C_R_LEAD, linewidth=1.6, zorder=7))
        ax.add_patch(Circle((c_pins, rr), 0.10, facecolor=C_R_LEAD, zorder=8))
        ax.text(c_body + 0.15, rr, lbl, ha="left", va="center",
                fontsize=9, color="white", fontweight="bold", zorder=9)
    # Etiqueta del chip horizontal debajo del cuerpo
    ax.text(c_body + body_w / 2, body_bot + 0.45, "IRLZ44N",
            ha="center", va="center", fontsize=9, color=C_LABEL,
            fontweight="bold")


def page_stripboard_3pin(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.suptitle("Stripboard 12 x 16 - cableado practico version 3-pin",
                  fontsize=15, fontweight="bold", y=0.96)
    fig.text(0.5, 0.93,
              "Perfboard 12 x 16, SIN cortes: MOSFET acostado (rotado 90, "
              "patillas dobladas) - cada pin G/D/S en su propia fila.",
              ha="center", fontsize=9, color=C_NOTE)

    ax = fig.add_axes([0.05, 0.34, 0.90, 0.58])
    ROWS, COLS = 12, 16
    draw_stripboard(ax, ROWS, COLS)

    # ── Asignacion de filas (MOSFET rotado 90: cada pin en su fila) ──
    # A (0)  +12V de entrada + catodo D1
    # B (1)  VCC fan (puente vertical a A en col 1)
    # C (2)  libre
    # D (3)  libre
    # E (4)  GATE MOSFET + GPIO 5 PWM (DIRECTO, sin puente)
    # F (5)  DRAIN MOSFET + GND fan + anodo D1 (DIRECTO, sin puente)
    # G (6)  SOURCE MOSFET + GND general (DIRECTO, sin puente)
    # H (7)  libre
    # I (8)  TACH NC
    # ── R1 vertical entre fila E (Gate) y fila G (GND)            ──
    # ── D1 vertical entre fila A (catodo) y fila F (anodo)        ──

    PAD_L_COL = 0
    PAD_R_COL = COLS - 1 - 2   # c=13
    # MOSFET rotado 90: patillas dobladas en col 6, cuerpo a la derecha
    MOSFET_PIN_COL = 6
    MOSFET_R_G = 4   # Gate en fila E
    MOSFET_R_D = 5   # Drain en fila F
    MOSFET_R_S = 6   # Source en fila G
    # D1 entre fila A (catodo, +12V) y fila F (anodo, GND fan) - 5 filas de salto
    D1_C = 11
    # R1 entre fila E (Gate) y fila G (GND) en col 3
    R1_C = 3

    # ── Fila A: +12V + catodo D1 ─────────────────────────────────
    strip_pad(ax, 0, PAD_L_COL, "+12V (fusible 1A)", C_WIRE_R, anchor="left")
    strip_wire(ax, 0, PAD_L_COL, D1_C, C_WIRE_R)

    # ── Fila B: VCC fan ──────────────────────────────────────────
    strip_pad(ax, 1, PAD_R_COL, "VCC fan (rojo)", C_WIRE_R, anchor="right")
    strip_wire(ax, 1, 1, PAD_R_COL, C_WIRE_R)
    # Puente vertical A1 ↔ B1 (junta +12V de la fila A con VCC fan)
    ax.add_line(Line2D([1, 1], [0, 1], color=C_WIRE_R, linewidth=2.4,
                       solid_capstyle="round", zorder=4))

    # ── Fila E: GATE + GPIO 5 PWM (todo directo en E) ───────────
    strip_pad(ax, MOSFET_R_G, PAD_L_COL,
              "GPIO 5 PWM\n(JP1 pin 15)", C_WIRE_G, anchor="left")
    strip_wire(ax, MOSFET_R_G, PAD_L_COL, MOSFET_PIN_COL, C_WIRE_G)

    # ── Fila F: DRAIN + GND fan + anodo D1 (todo directo en F) ──
    strip_pad(ax, MOSFET_R_D, PAD_R_COL, "GND fan (negro)", C_WIRE_K,
              anchor="right")
    strip_wire(ax, MOSFET_R_D, MOSFET_PIN_COL, PAD_R_COL, C_WIRE_K)

    # ── Fila G: SOURCE + GND general (todo directo en G) ────────
    strip_pad(ax, MOSFET_R_S, PAD_L_COL, "GND (autocarav.)", C_WIRE_K,
              anchor="left")
    strip_wire(ax, MOSFET_R_S, PAD_L_COL, MOSFET_PIN_COL, C_WIRE_K)

    # ── MOSFET TO-220 rotado 90: patillas en col 6, cuerpo a la derecha
    strip_mosfet_to220_rot90(ax, MOSFET_R_G, MOSFET_R_D, MOSFET_R_S,
                              MOSFET_PIN_COL)

    # ── R1 vertical entre fila E (Gate) y fila G (GND) en col R1_C ──
    strip_v_resistor(ax, MOSFET_R_G, MOSFET_R_S, R1_C,
                     "R1 = 10 k (pull-down)", label_pos="top")

    # ── D1 vertical entre fila A (catodo) y fila F (anodo) ──────
    strip_v_diode(ax, 0, MOSFET_R_D, D1_C, "D1 = 1N5819\n(flyback)")

    # ── Fila I: TACH NC ──────────────────────────────────────────
    strip_pad(ax, 8, PAD_R_COL, "TACH fan (NC)", C_WIRE_Y, anchor="right")
    ax.add_line(Line2D([COLS - 4, COLS - 1 - 2], [8, 8], color=C_WIRE_Y,
                       linewidth=2.4, linestyle="--", zorder=4))
    ax.text(COLS - 4 - 0.5, 8, "X", ha="right", va="center",
            fontsize=13, color=C_CUT, fontweight="bold")

    # ── Leyenda y notas ──────────────────────────────────────────
    notes_ax = fig.add_axes([0.05, 0.03, 0.90, 0.27])
    notes_ax.set_axis_off()
    notes_ax.text(0.0, 1.0, "Leyenda:", fontsize=11, fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    y = 0.88
    for color, label in [
        (C_WIRE_R, "Rojo: +12V autocaravana / VCC fan"),
        (C_WIRE_K, "Negro: GND comun / drain-source MOSFET"),
        (C_WIRE_G, "Verde: GPIO 5 PWM (3.3V) -> Gate MOSFET"),
        (C_WIRE_Y, "Amarillo: TACH fan (NC, opcional para leer RPM)"),
    ]:
        notes_ax.plot([0.02, 0.07], [y, y], color=color,
                       linewidth=3, transform=notes_ax.transAxes)
        notes_ax.text(0.09, y, label, fontsize=9,
                       transform=notes_ax.transAxes, va="center")
        y -= 0.08

    notes_ax.text(0.0, 0.50, "Notas criticas:", fontsize=11,
                  fontweight="bold",
                  transform=notes_ax.transAxes, va="top")
    y = 0.42
    for txt in [
        "1. MONTAR el MOSFET ACOSTADO (rotado 90): doblar las 3 patillas 90 "
           "hacia abajo de modo que cada pin (G/D/S) entre en una fila "
           "DISTINTA del stripboard. Asi cada pin queda aislado en su tira "
           "y no hace falta ningun corte.",
        "2. Pull-down R1 a GND: sin el, con GPIO 5 flotante (reset/boot) el "
           "fan podria activarse erratico.",
        "3. Diodo D1 en paralelo al fan (catodo a +12V, anodo a GND del fan) "
           "absorbe el kick inductivo al apagar el motor; sin el, el MOSFET puede "
           "danarse a la larga.",
        "4. GND comun entre la masa de la autocaravana y el GND del 7\" "
           "OBLIGATORIO: sin esa referencia comun la senial PWM oscila respecto a "
           "una tierra distinta.",
        "5. Fusible 1A en la linea +12V (no dibujado en el stripboard: va aguas "
           "arriba, en el cuadro electrico de la autocaravana).",
    ]:
        notes_ax.text(0.02, y, txt, fontsize=8.5,
                       transform=notes_ax.transAxes, va="top", color=C_LABEL,
                       wrap=True)
        y -= 0.08

    pdf.savefig(fig)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════
# main
# ════════════════════════════════════════════════════════════════

def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with PdfPages(OUT) as pdf:
        page_4pin(pdf)
        page_3pin(pdf)
        page_stripboard_3pin(pdf)
    print(f"OK -> {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
