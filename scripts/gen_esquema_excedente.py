#!/usr/bin/env python3
"""Genera docs/esquema_excedente_solar.png (y .pdf): como se conecta el mando del
P4 al rele del frigo por excedente solar, conviviendo con la D+ del vehiculo.

Uso:
    python3 scripts/gen_esquema_excedente.py

Es el circuito del diseno docs/superpowers/specs/2026-07-17-frigo-excedente-solar-
design.md: GPIO1 (JP1 pin 7) -> rele piloto -> diodo -> bobina del rele tocho, con
la D+ del vehiculo entrando en el MISMO nodo. El diodo impide que la D+ (que con
el motor en marcha esta mas alta) empuje corriente hacia el piloto.

Ojo al dibujar: el +12 V que conmuta el rele tocho sale del BUS de habitaculo, no
del nodo del diodo. Y la D+ es solo una senal de mando (nunca una fuente).

Se dibuja a 3x y se reduce con LANCZOS: PIL no suaviza las lineas.
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

SS = 3
W, H = 1680, 1200

C_FONDO = (255, 255, 255)
C_TINTA = (26, 32, 44)
C_SUAVE = (108, 122, 137)
C_12V   = (198, 40, 40)
C_MASA  = (38, 38, 38)
C_DP    = (21, 101, 192)
C_MANDO = (239, 108, 0)
C_CAJA  = (247, 249, 252)
C_BORDE = (176, 190, 205)
C_AVISO = (255, 248, 225)
C_AVISO_B = (230, 190, 80)

FUENTE = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FUENTE_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

img = Image.new("RGB", (W * SS, H * SS), C_FONDO)
d = ImageDraw.Draw(img)


def f(px, negrita=False):
    return ImageFont.truetype(FUENTE_B if negrita else FUENTE, int(px * SS))


def esc(v):
    return int(round(v * SS))


def texto(x, y, s, px=15, color=C_TINTA, negrita=False, anchor="la"):
    d.text((esc(x), esc(y)), s, font=f(px, negrita), fill=color, anchor=anchor)


def caja(x0, y0, x1, y1, titulo=None, sub=None, borde=C_BORDE, relleno=C_CAJA,
         grosor=2.2, titulo_px=16):
    d.rounded_rectangle([esc(x0), esc(y0), esc(x1), esc(y1)], radius=esc(10),
                        fill=relleno, outline=borde, width=esc(grosor))
    cy = (y0 + y1) / 2
    if sub:
        cy -= 11
    if titulo:
        texto((x0 + x1) / 2, cy, titulo, titulo_px, C_TINTA, True, "mm")
    if sub:
        texto((x0 + x1) / 2, cy + 20, sub, 12.5, C_SUAVE, False, "mm")


def cable(puntos, color, grosor=2.6):
    d.line([(esc(x), esc(y)) for x, y in puntos], fill=color, width=esc(grosor),
           joint="curve")


def trazos(puntos, color=C_SUAVE, grosor=1.4, largo=7, hueco=5):
    """Linea a trazos: PIL no tiene dash, se dibuja a mano."""
    for (x0, y0), (x1, y1) in zip(puntos, puntos[1:]):
        dx, dy = x1 - x0, y1 - y0
        dist = (dx * dx + dy * dy) ** 0.5
        if dist == 0:
            continue
        ux, uy = dx / dist, dy / dist
        t = 0.0
        while t < dist:
            fin = min(t + largo, dist)
            d.line([(esc(x0 + ux * t), esc(y0 + uy * t)),
                    (esc(x0 + ux * fin), esc(y0 + uy * fin))],
                   fill=color, width=esc(grosor))
            t += largo + hueco


def nodo(x, y, color=C_TINTA, r=4.4):
    d.ellipse([esc(x - r), esc(y - r), esc(x + r), esc(y + r)], fill=color)


def flecha(x, y, direccion="derecha", color=C_TINTA, tam=8):
    if direccion == "derecha":
        pts = [(x, y), (x - tam, y - tam * 0.6), (x - tam, y + tam * 0.6)]
    elif direccion == "abajo":
        pts = [(x, y), (x - tam * 0.6, y - tam), (x + tam * 0.6, y - tam)]
    elif direccion == "arriba":
        pts = [(x, y), (x - tam * 0.6, y + tam), (x + tam * 0.6, y + tam)]
    else:
        pts = [(x, y), (x + tam, y - tam * 0.6), (x + tam, y + tam * 0.6)]
    d.polygon([(esc(a), esc(b)) for a, b in pts], fill=color)


def diodo(x, y, vertical=False, color=C_TINTA, etiqueta=None):
    t, a = 12, 10
    if vertical:
        d.polygon([(esc(x - a), esc(y - t)), (esc(x + a), esc(y - t)), (esc(x), esc(y + t))],
                  fill=color)
        d.line([(esc(x - a), esc(y + t)), (esc(x + a), esc(y + t))], fill=color, width=esc(3.6))
    else:
        d.polygon([(esc(x - t), esc(y - a)), (esc(x - t), esc(y + a)), (esc(x + t), esc(y))],
                  fill=color)
        d.line([(esc(x + t), esc(y - a)), (esc(x + t), esc(y + a))], fill=color, width=esc(3.6))
    if etiqueta:
        texto(x, y - 26, etiqueta, 13, C_TINTA, True, "mm")


def fusible(x, y, etiqueta=None):
    d.rounded_rectangle([esc(x - 17), esc(y - 8), esc(x + 17), esc(y + 8)], radius=esc(4),
                        outline=C_TINTA, width=esc(2.2), fill=C_FONDO)
    cable([(x - 17, y), (x + 17, y)], C_TINTA, 2.2)
    if etiqueta:
        texto(x + 26, y, etiqueta, 13, C_TINTA, True, "lm")


def masa(x, y):
    cable([(x, y), (x, y + 14)], C_MASA, 2.6)
    for i, ancho in enumerate((15, 9, 4)):
        d.line([(esc(x - ancho), esc(y + 14 + i * 6)), (esc(x + ancho), esc(y + 14 + i * 6))],
               fill=C_MASA, width=esc(2.4))


def terminal(x, y, etiqueta, arriba=True, color=C_TINTA):
    nodo(x, y, color, 3.8)
    texto(x, y - 16 if arriba else y + 16, etiqueta, 12.5, C_SUAVE, True, "mm")


# ── Titulo ───────────────────────────────────────────────────────────────────
texto(60, 40, "Excedente solar  →  relé del frigo", 30, C_TINTA, True)
texto(60, 82, "El P4 cierra la bobina del relé tocho por un diodo. La D+ del vehículo entra en el mismo nodo,", 15.5, C_SUAVE)
texto(60, 104, "así que el frigo va a 12 V con el motor en marcha o cuando sobra sol, sin que las dos cosas se peleen.", 15.5, C_SUAVE)

# ── Leyenda ──────────────────────────────────────────────────────────────────
lx, ly = 1290, 44
for i, (col, txt) in enumerate([(C_12V, "+12 V habitáculo"), (C_DP, "D+ del vehículo"),
                                (C_MANDO, "mando del P4"), (C_MASA, "masa")]):
    cable([(lx, ly + i * 23), (lx + 26, ly + i * 23)], col, 3.2)
    texto(lx + 34, ly + i * 23, txt, 13, C_SUAVE, False, "lm")

# ── Bus de +12 V (arriba) ────────────────────────────────────────────────────
BUS_Y = 200
cable([(210, BUS_Y), (1320, BUS_Y)], C_12V, 3.4)
nodo(210, BUS_Y, C_12V)
texto(200, BUS_Y, "+12 V habitáculo", 14, C_12V, True, "rm")
cable([(380, BUS_Y), (380, 258)], C_12V, 3.2)          # bajada 1: fusible -> piloto
fusible(380, 258, "5 A")
cable([(380, 266), (380, 330)], C_12V, 3.2)
cable([(1310, BUS_Y), (1310, 330), (1216, 330), (1216, 424)], C_12V, 3.2)   # bajada 2: contacto 30
texto(1318, 336, "al contacto 30", 12.5, C_12V, True, "lm")

# ── P4 y rele piloto ─────────────────────────────────────────────────────────
caja(60, 380, 240, 480, "P4  7\"", "GPIO1 · JP1 pin 7", borde=(210, 150, 90))
cable([(240, 430), (300, 430)], C_MANDO, 2.8)
flecha(298, 430, "derecha", C_MANDO)
texto(272, 452, "3,3 V", 12.5, C_MANDO, True, "mm")

PX0, PY0, PX1, PY1 = 300, 330, 560, 500
caja(PX0, PY0, PX1, PY1, "RELÉ PILOTO", None, grosor=2.4)
texto(PX0 + 16, PY0 + 22, "entrada", 12, C_SUAVE)
cable([(380, PY0), (380, PY0 + 22)], C_12V, 3.2)        # entra la alimentacion
cable([(380, PY0 + 22), (380, PY0 + 40)], C_12V, 3.2)
# contacto NA dentro del piloto
cable([(364, 420), (398, 420)], C_TINTA, 2.6)
nodo(398, 420, C_TINTA, 3.6)
cable([(420, 404), (500, 404)], C_TINTA, 2.8)
nodo(420, 404, C_TINTA, 3.6)
nodo(500, 404, C_TINTA, 3.6)
texto(430, 428, "NA", 11.5, C_SUAVE)
texto((PX0 + PX1) / 2, PY1 - 26, "contacto del mando solar", 12, C_SUAVE, False, "mm")

# salida del piloto -> diodo
cable([(500, 404), (620, 404), (620, 430)], C_MANDO, 2.8)
cable([(620, 430), (688, 430)], C_MANDO, 2.8)
diodo(704, 430, False, C_TINTA, "1N4007")
cable([(720, 430), (900, 430)], C_MANDO, 2.8)
texto(704, 468, "anillo → bobina", 12.5, C_TINTA, True, "mm")

# ── D+ (entra por abajo, al mismo nodo) ──────────────────────────────────────
cable([(760, 700), (760, 430)], C_DP, 3.0)
nodo(760, 430, C_TINTA)
nodo(760, 700, C_DP)                                  # de aqui sale la rama al frigo
cable([(760, 700), (1400, 700), (1400, 500)], C_DP, 3.0)
flecha(1400, 502, "arriba", C_DP, 9)
texto(736, 660, "D+ del vehículo  (azul, 1–1,5 mm²)", 13.5, C_DP, True, "rm")
texto(736, 682, "solo con el motor en marcha y cargando", 12.5, C_SUAVE, False, "rm")
texto(1080, 726, "y la MISMA D+ entra en el frigo: es lo que le hace cambiar de energía", 13, C_DP, True, "mm")

# ── Bobina del rele tocho ────────────────────────────────────────────────────
BX0, BY0, BX1, BY1 = 900, 380, 1040, 480
caja(BX0, BY0, BX1, BY1, "BOBINA", "85 / 86", borde=(120, 140, 160), grosor=2.6)
texto((BX0 + BX1) / 2, BY0 - 26, "RELÉ TOCHO", 13, C_TINTA, True, "mm")
cable([(760, 430), (BX0, 430)], C_MANDO, 2.8)
cable([(BX1, 430), (1120, 430)], C_TINTA, 2.6)
nodo(1120, 430, C_TINTA)

# rueda libre en paralelo con la bobina: lazo por debajo
cable([(860, 430), (860, 560), (990, 560)], C_SUAVE, 1.8)
diodo(1006, 560, False, C_SUAVE)
cable([(1022, 560), (1120, 560), (1120, 430)], C_SUAVE, 1.8)
texto(975, 604, "rueda libre (1N4148)", 12, C_SUAVE, False, "mm")

# masa de la bobina: baja desde el lazo (mismo nodo), sin cruzar nada
cable([(1120, 560), (1120, 596)], C_MASA, 2.6)
masa(1120, 596)
texto(1142, 610, "masa", 12.5, C_SUAVE, False, "lm")

# ── Contactos del rele tocho y frigo ─────────────────────────────────────────
# 30 (de la bajada de +12 V) y 87 (al frigo), con el brazo abierto
nodo(1216, 424, C_TINTA, 3.8)
cable([(1216, 424), (1230, 424)], C_TINTA, 2.6)
cable([(1232, 418), (1302, 392)], C_TINTA, 3.2)          # brazo: contacto abierto
nodo(1330, 424, C_TINTA, 3.8)
cable([(1302, 410), (1330, 424)], C_TINTA, 2.0)
texto(1196, 424, "30", 12.5, C_SUAVE, True, "rm")
texto(1330, 400, "87", 12.5, C_SUAVE, True, "mm")
# enlace mecanico del rele (a trazos: no es un cable)
trazos([(1040, 396), (1160, 396), (1160, 410)])
texto(1150, 374, "enlace mecánico", 11.5, C_SUAVE, False, "mm")

cable([(1330, 424), (1330, 430), (1470, 430)], C_12V, 4.0)
flecha(1467, 430, "derecha", C_12V, 9)
texto(1480, 348, "12 V al frigo · 8–11 A", 13, C_12V, True, "mm")
caja(1470, 380, 1650, 500, "FRIGO", "automático (AES) · resistencia 12 V", borde=(210, 150, 90))
nodo(1400, 500, C_DP, 3.8)
texto(1414, 468, "D+", 12.5, C_DP, True, "rm")
texto(1414, 492, "12 V", 12.5, C_12V, True, "rm")
cable([(1500, 480), (1500, 536)], C_MASA, 2.4)
masa(1500, 536)
texto(1522, 548, "masa chasis", 12.5, C_SUAVE, False, "lm")

texto(1560, 600, "El relé tocho es el de siempre:", 12.5, C_SUAVE, False, "mm")
texto(1560, 620, "el P4 no alimenta el frigo", 12.5, C_SUAVE, False, "mm")

# ── Panel: quien cierra el rele ──────────────────────────────────────────────
RX0, RY0, RX1, RY1 = 60, 820, 800, 1130
d.rounded_rectangle([esc(RX0), esc(RY0), esc(RX1), esc(RY1)], radius=esc(12),
                    fill=C_AVISO, outline=C_AVISO_B, width=esc(2))
texto(RX0 + 24, RY0 + 20, "Qué cierra el relé en cada caso", 16, C_TINTA, True)
filas = [
    ("Motor en marcha", "la D+ (el P4 ni interviene)", C_DP),
    ("Parado, con sol y SoC suficiente", "el P4, por el diodo", C_MANDO),
    ("Las dos cosas a la vez", "indistinto: el diodo evita que se peleen", C_TINTA),
    ("Aparece 230 V en modo solar", "el P4 abre; si hay motor, sigue la D+", C_TINTA),
]
for i, (a, b, col) in enumerate(filas):
    y = RY0 + 62 + i * 38
    d.ellipse([esc(RX0 + 26), esc(y - 5), esc(RX0 + 36), esc(y + 5)], fill=col)
    texto(RX0 + 50, y, a, 13.5, C_TINTA, True, "lm")
    texto(RX0 + 320, y, b, 13, C_SUAVE, False, "lm")

# ── Panel: recuerda ──────────────────────────────────────────────────────────
AX0, AY0, AX1, AY1 = 840, 820, 1650, 1130
d.rounded_rectangle([esc(AX0), esc(AY0), esc(AX1), esc(AY1)], radius=esc(12),
                    outline=C_BORDE, width=esc(2), fill=C_CAJA)
texto(AX0 + 24, AY0 + 20, "Recuerda", 16, C_TINTA, True)
avisos = [
    "• El diodo va con el anillo (cátodo) hacia la bobina.",
    "• Fusible cerca de la batería: protege el cable, no el relé.",
    "• Nunca cuelgues el frigo del piloto: los 8–11 A son del relé tocho.",
    "• Si el relé tocho no trae rueda libre, ponla tú (1N4148 en paralelo).",
    "• La D+ es una señal de mando: no la uses como fuente de corriente.",
    "• Con el motor en marcha, el diodo impide que la D+ retroalimente el piloto.",
    "• Un frigo automático (AES) NO cambia a 12 V por tener 12 V: cambia al ver la D+.",
    "• Por eso la D+ va a los dos sitios: a la bobina del relé y a la entrada D+ del frigo.",
]
for i, a in enumerate(avisos):
    texto(AX0 + 24, AY0 + 56 + i * 30, a, 13, C_TINTA)

img = img.resize((W, H), Image.LANCZOS)
salida = Path(__file__).resolve().parent.parent / "docs" / "esquema_excedente_solar.png"
img.save(salida)
img.save(salida.with_suffix(".pdf"), "PDF", resolution=150.0)
print("PNG:", salida)
print("PDF:", salida.with_suffix(".pdf"))
