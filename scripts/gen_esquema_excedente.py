#!/usr/bin/env python3
"""Genera docs/esquema_excedente_solar.png (y .pdf): como se conecta el mando del
P4 al frigo por excedente solar en la furgo (NE185-11S, frigo AES sin salida AES
en el regulador solar).

Uso:
    python3 scripts/gen_esquema_excedente.py

Dos circuitos distintos, y ahi esta la clave:

- SEÑAL: la entrada D+/S+ del frigo recibe dos cosas. Del NE185 (J4, salida D+ de
  0,5 A) cuando el motor esta en marcha, y del P4 (contacto del rele piloto ->
  fusible -> diodo) cuando sobra sol. El diodo impide que el P4 meta corriente en
  la salida del NE185, y que la D+ del alternador entre al piloto.
- POTENCIA (8-11 A de la resistencia): de serie sale del NE185 (JP4-2, F2 20 A,
  bateria del VEHICULO). Con sol hay que CONMUTAR a la bateria de servicios, nunca
  ponerlas en paralelo: uniria los dos bancos saltandose el DC-DC.

Los datos de terminales son del manual de la unidad (documentacion/ne185_manuales/
NE185-11S.pdf, pagina 17) y del manual del frigo (Dometic RMx8xxx, apartado 4.9.4).

Se dibuja a 3x y se reduce con LANCZOS: PIL no suaviza las lineas.
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

SS = 3
W, H = 1780, 1220

C_FONDO = (255, 255, 255)
C_TINTA = (26, 32, 44)
C_SUAVE = (108, 122, 137)
C_SERV  = (198, 40, 40)        # +12 V bateria de servicios
C_VEH   = (140, 82, 40)        # +12 V bateria del vehiculo
C_SENAL = (21, 101, 192)       # D+ / S+ (senal)
C_MANDO = (239, 108, 0)        # mando del P4 (GPIO1)
C_MASA  = (38, 38, 38)
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
         grosor=2.2, titulo_px=16, sub_px=12.5):
    d.rounded_rectangle([esc(x0), esc(y0), esc(x1), esc(y1)], radius=esc(10),
                        fill=relleno, outline=borde, width=esc(grosor))
    cy = (y0 + y1) / 2
    if sub:
        cy -= 11
    if titulo:
        texto((x0 + x1) / 2, cy, titulo, titulo_px, C_TINTA, True, "mm")
    if sub:
        texto((x0 + x1) / 2, cy + 20, sub, sub_px, C_SUAVE, False, "mm")


def cable(puntos, color, grosor=2.6):
    d.line([(esc(x), esc(y)) for x, y in puntos], fill=color, width=esc(grosor),
           joint="curve")


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


def diodo(x, y, color=C_TINTA):
    t, a = 12, 10
    d.polygon([(esc(x - t), esc(y - a)), (esc(x - t), esc(y + a)), (esc(x + t), esc(y))],
              fill=color)
    d.line([(esc(x + t), esc(y - a)), (esc(x + t), esc(y + a))], fill=color, width=esc(3.6))


def fusible(x, y, etiqueta=None, vertical=False):
    if vertical:
        d.rounded_rectangle([esc(x - 8), esc(y - 15), esc(x + 8), esc(y + 15)], radius=esc(4),
                            outline=C_TINTA, width=esc(2.2), fill=C_FONDO)
        cable([(x, y - 15), (x, y + 15)], C_TINTA, 2.2)
        if etiqueta:
            texto(x + 20, y, etiqueta, 12.5, C_TINTA, True, "lm")
    else:
        d.rounded_rectangle([esc(x - 16), esc(y - 8), esc(x + 16), esc(y + 8)], radius=esc(4),
                            outline=C_TINTA, width=esc(2.2), fill=C_FONDO)
        cable([(x - 16, y), (x + 16, y)], C_TINTA, 2.2)
        if etiqueta:
            texto(x, y - 22, etiqueta, 12.5, C_TINTA, True, "mm")


def masa(x, y):
    cable([(x, y), (x, y + 12)], C_MASA, 2.4)
    for i, ancho in enumerate((14, 8, 4)):
        d.line([(esc(x - ancho), esc(y + 12 + i * 6)), (esc(x + ancho), esc(y + 12 + i * 6))],
               fill=C_MASA, width=esc(2.4))


# ── Titulo y leyenda ─────────────────────────────────────────────────────────
texto(60, 38, "Excedente solar  →  frigo   (NE185-11S, frigo AES)", 28, C_TINTA, True)
texto(60, 78, "Dos circuitos: la SEÑAL (entrada D+/S+ del frigo) y la POTENCIA (8-11 A de la resistencia).", 15, C_SUAVE)
texto(60, 100, "Con sol, el P4 da la señal S+ y conmuta la potencia a la batería de servicios. Nunca las dos baterías a la vez.", 15, C_SUAVE)

lx, ly = 1330, 40
for i, (col, txt) in enumerate([(C_SENAL, "D+ / S+  (señal)"), (C_MANDO, "mando del P4"),
                                (C_SERV, "+12 V batería servicios"), (C_VEH, "+12 V batería vehículo"),
                                (C_MASA, "masa")]):
    cable([(lx, ly + i * 22), (lx + 26, ly + i * 22)], col, 3.2)
    texto(lx + 34, ly + i * 22, txt, 12.5, C_SUAVE, False, "lm")

Y_SENAL = 374
Y_POT = 560
Y_GAS = 700

# ── NE185: de donde sale todo ────────────────────────────────────────────────
NX0, NY0, NX1, NY1 = 60, 300, 320, 800
caja(NX0, NY0, NX1, NY1, "NE185", "centralita (11S)", borde=(120, 140, 160), grosor=2.6)
texto(NX0 + 18, Y_SENAL, "J4", 13, C_SENAL, True)
texto(NX0 + 52, Y_SENAL, "salida D+ (0,5 A)", 12, C_SUAVE, False, "lm")
texto(NX0 + 18, Y_POT + 22, "JP4-2", 13, C_VEH, True)
texto(NX0 + 70, Y_POT + 22, "12 V del frigo (F2 20 A)", 12, C_SUAVE, False, "lm")
texto(NX0 + 18, Y_GAS, "JP4-3", 13, C_SUAVE, True)
texto(NX0 + 70, Y_GAS, "encendido de gas (F7)", 12, C_SUAVE, False, "lm")
texto(NX0 + 18, NY1 - 58, "JP13 = ENTRADA de D+", 12.5, C_TINTA, True)
texto(NX0 + 18, NY1 - 40, "no se toca", 12, C_SUAVE)

# ── SEÑAL: P4 -> piloto -> fusible -> diodo -> nodo ──────────────────────────
caja(370, 300, 520, 400, "P4", "GPIO1 · JP1 pin 7", borde=(210, 150, 90))
texto(445, 424, "3,3 V", 12, C_MANDO, True, "mm")

PX0, PY0, PX1, PY1 = 600, 296, 800, 430
caja(PX0, PY0, PX1, PY1, "RELÉ PILOTO", "contacto NA", grosor=2.4)
cable([(520, 350), (560, 350), (560, 396), (PX0, 396)], C_MANDO, 2.8)
flecha(PX0 - 4, 396, "derecha", C_MANDO)
cable([(630, 396), (658, 396)], C_TINTA, 2.4)
nodo(658, 396, C_TINTA, 3.4)
cable([(680, 382), (752, 382)], C_TINTA, 2.6)
nodo(752, 382, C_TINTA, 3.4)
cable([(752, 382), (860, 382), (860, Y_SENAL - 15)], C_MANDO, 2.8)
fusible(860, Y_SENAL, "5 A", vertical=True)
cable([(860, Y_SENAL + 15), (860, 462)], C_MANDO, 2.8)
cable([(860, 462), (940, 462)], C_MANDO, 2.8)
diodo(956, 462, C_TINTA)
cable([(968, 462), (1060, 462), (1060, Y_SENAL)], C_MANDO, 2.8)
cable([(1060, Y_SENAL), (1180, Y_SENAL)], C_SENAL, 2.8)
nodo(1180, Y_SENAL, C_TINTA)
texto(956, 494, "diodo (anillo hacia el frigo)", 12, C_SUAVE, False, "mm")

# el J4 del NE185 entra en ese mismo nodo
cable([(NX1, Y_SENAL), (1180, Y_SENAL)], C_SENAL, 2.8)
texto(NX1 + 16, Y_SENAL + 20, "señal del NE185 (motor) por J4", 12.5, C_SENAL, True, "lm")

# ── POTENCIA: conmutacion ────────────────────────────────────────────────────
RX0, RY0, RX1, RY1 = 700, 500, 1010, 664
caja(RX0, RY0, RX1, RY1, "RELÉ DE CONMUTACIÓN", "12 V · 20 A · reposo/trabajo",
     borde=(120, 140, 160), grosor=2.4, sub_px=12)
cable([(RX0, 592), (RX0 + 30, 592)], C_VEH, 2.6)
cable([(RX0, 640), (RX0 + 30, 640)], C_SERV, 2.6)
nodo(RX0 + 30, 592, C_VEH, 3.4)
nodo(RX0 + 30, 640, C_SERV, 3.4)
cable([(RX0 + 48, 600), (RX0 + 100, 614)], C_TINTA, 3.0)
cable([(RX0 + 100, 614), (RX1, 614)], C_TINTA, 2.6)
nodo(RX1, 614, C_TINTA, 3.6)
texto(RX0 + 26, 574, "NC", 11.5, C_SUAVE, True, "lm")
texto(RX0 + 26, 664, "NO", 11.5, C_SUAVE, True, "lm")
texto((RX0 + RX1) / 2, RY1 - 26, "bobina: la manda el P4 (misma señal que el S+)", 11.5, C_MANDO, True, "mm")

# NE185 JP4-2 -> NC (reposo)
cable([(NX1, Y_POT + 22), (500, Y_POT + 22), (500, 592), (RX0, 592)], C_VEH, 2.8)
texto(510, Y_POT - 6, "12 V del vehículo (de serie)", 12.5, C_VEH, True, "lm")

# +12 V servicios -> NO (trabajo)
nodo(560, 736, C_SERV)
texto(560, 722, "+12 V batería de servicios", 12.5, C_SERV, True, "lm")
cable([(560, 736), (560, 640), (RX0, 640)], C_SERV, 3.0)
fusible(560, 690, "20 A", vertical=True)

# mando de la bobina desde el contacto del piloto
cable([(690, 396), (690, 470), (648, 470), (648, 520), (RX0, 520)], C_MANDO, 2.0)

# COM -> frigo
cable([(RX1, 614), (1180, 614)], C_SERV, 3.6)
flecha(1177, 614, "derecha", C_SERV, 9)

# ── FRIGO ────────────────────────────────────────────────────────────────────
FX0, FX1 = 1190, 1600
caja(FX0, 300, FX1, 424, "FRIGO", "AES · electrónica propia", borde=(210, 150, 90))
caja(FX0, 540, FX1, 700, "FRIGO", "resistencia 12 V · 8-11 A", borde=(210, 150, 90))
texto(1180, Y_SENAL - 18, "D+/S+", 12.5, C_SENAL, True, "rm")
texto(1180, 640, "12 V", 12.5, C_SERV, True, "rm")
cable([(NX1, Y_GAS), (1180, Y_GAS)], C_SUAVE, 2.0)
texto(1180, Y_GAS + 16, "gas (F7) — no tocar", 12, C_SUAVE, False, "mm")
cable([(1180, 424), (1180, Y_SENAL)], C_SENAL, 2.2)

cable([(1395, 700), (1395, 748)], C_MASA, 2.4)
masa(1395, 748)
texto(1415, 762, "masa chasis", 12, C_SUAVE, False, "lm")

# ── Paneles ──────────────────────────────────────────────────────────────────
PX0b, PY0b, PX1b, PY1b = 60, 840, 840, 1170
d.rounded_rectangle([esc(PX0b), esc(PY0b), esc(PX1b), esc(PY1b)], radius=esc(12),
                    fill=C_AVISO, outline=C_AVISO_B, width=esc(2))
texto(PX0b + 24, PY0b + 20, "Cómo se comporta", 16, C_TINTA, True)
filas = [
    ("Motor en marcha", "el NE185 manda su D+ (J4) y sus 12 V de serie (JP4-2)", C_VEH),
    ("Parado, con sol y SoC suficiente", "el P4 da S+ y conmuta la potencia a servicios", C_SERV),
    ("Parado, sin sol", "ni señal ni 12 V: el frigo se queda en gas", C_SUAVE),
    ("Aparece 230 V en modo solar", "el P4 suelta: vuelve la D+ del NE185", C_TINTA),
]
for i, (a, b, col) in enumerate(filas):
    y = PY0b + 62 + i * 40
    d.ellipse([esc(PX0b + 26), esc(y - 5), esc(PX0b + 36), esc(y + 5)], fill=col)
    texto(PX0b + 50, y, a, 13, C_TINTA, True, "lm")
    texto(PX0b + 330, y, b, 12.5, C_SUAVE, False, "lm")

AX0, AY0, AX1, AY1 = 880, 840, 1720, 1170
d.rounded_rectangle([esc(AX0), esc(AY0), esc(AX1), esc(AY1)], radius=esc(12),
                    outline=C_BORDE, width=esc(2), fill=C_CAJA)
texto(AX0 + 24, AY0 + 20, "Recuerda", 16, C_TINTA, True)
avisos = [
    "• Las dos baterías NUNCA en paralelo: la conmutación es de reposo/trabajo.",
    "• La señal lleva un diodo: el P4 no puede meter corriente en la salida del NE185.",
    "• D+ y S+ son señales: cable de ~1 mm². La potencia, cable de 20 A.",
    "• Fusible de 20 A para la potencia, cerca de la batería de servicios.",
    "• El encendido de gas (JP4-3, F7) no se toca, o el frigo no arranca a gas.",
    "• Vale si el frigo está en la salida POR RELÉ (JP4-2). Comprueba en cuál está.",
]
for i, a in enumerate(avisos):
    texto(AX0 + 24, AY0 + 58 + i * 30, a, 12.5, C_TINTA)

img = img.resize((W, H), Image.LANCZOS)
salida = Path(__file__).resolve().parent.parent / "docs" / "esquema_excedente_solar.png"
img.save(salida)
img.save(salida.with_suffix(".pdf"), "PDF", resolution=150.0)
print("PNG:", salida)
print("PDF:", salida.with_suffix(".pdf"))
