#!/usr/bin/env python3
"""Genera docs/esquema_excedente_solar.png (y .pdf): como se conecta el mando del
P4 al frigo por excedente solar en la furgo (NE185-11S, frigo AES).

Uso:
    python3 scripts/gen_esquema_excedente.py

Son DOS circuitos distintos y se dibujan por separado, que es como se leen:

  1. SEÑAL (D+/S+): la entrada del frigo recibe la D+ del NE185 (J4, 0,5 A) con el
     motor, y la S+ del P4 (contacto del rele piloto -> fusible -> diodo) cuando
     sobra sol. El diodo impide que el P4 meta corriente en la salida del NE185.
  2. POTENCIA (8-11 A): de serie sale del NE185 (JP4-2, F2 20 A, bateria del
     VEHICULO). Con sol hay que CONMUTAR a la bateria de servicios con un rele de
     reposo/trabajo. Nunca en paralelo: uniria los dos bancos saltandose el DC-DC.

Datos: manual de la unidad (documentacion/ne185_manuales/NE185-11S.pdf, pag. 17) y
manual del frigo (Dometic RMx8xxx, apartado 4.9.4).

Se dibuja a 3x y se reduce con LANCZOS: PIL no suaviza las lineas.
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

SS = 3
W, H = 1800, 1490

C_FONDO = (255, 255, 255)
C_TINTA = (26, 32, 44)
C_SUAVE = (110, 124, 139)
C_SERV  = (198, 40, 40)        # +12 V bateria de servicios
C_VEH   = (146, 88, 42)        # +12 V bateria del vehiculo
C_SENAL = (21, 101, 192)       # D+ / S+ (senal)
C_MANDO = (239, 108, 0)        # mando del P4 (GPIO1)
C_MASA  = (38, 38, 38)
C_CAJA  = (248, 250, 253)
C_BORDE = (176, 190, 205)
C_MARCO = (226, 232, 240)
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


def texto(x, y, s, px=16, color=C_TINTA, negrita=False, anchor="la"):
    d.text((esc(x), esc(y)), s, font=f(px, negrita), fill=color, anchor=anchor)


def marco(x0, y0, x1, y1, titulo, color_titulo=C_TINTA, sub=None):
    d.rounded_rectangle([esc(x0), esc(y0), esc(x1), esc(y1)], radius=esc(16),
                        outline=C_MARCO, width=esc(3), fill=(252, 253, 255))
    texto(x0 + 28, y0 + 22, titulo, 21, color_titulo, True)
    if sub:
        texto(x0 + 28, y0 + 52, sub, 15, C_SUAVE)


def caja(x0, y0, x1, y1, titulo, sub=None, borde=C_BORDE, grosor=2.4, titulo_px=17,
         sub_px=14, relleno=C_CAJA):
    d.rounded_rectangle([esc(x0), esc(y0), esc(x1), esc(y1)], radius=esc(12),
                        fill=relleno, outline=borde, width=esc(grosor))
    cy = (y0 + y1) / 2
    if sub:
        cy -= 12
    texto((x0 + x1) / 2, cy, titulo, titulo_px, C_TINTA, True, "mm")
    if sub:
        texto((x0 + x1) / 2, cy + 22, sub, sub_px, C_SUAVE, False, "mm")


def cable(puntos, color, grosor=3.0):
    d.line([(esc(x), esc(y)) for x, y in puntos], fill=color, width=esc(grosor),
           joint="curve")


def nodo(x, y, color=C_TINTA, r=5.0):
    d.ellipse([esc(x - r), esc(y - r), esc(x + r), esc(y + r)], fill=color)


def flecha(x, y, direccion="derecha", color=C_TINTA, tam=9):
    if direccion == "derecha":
        pts = [(x, y), (x - tam, y - tam * 0.6), (x - tam, y + tam * 0.6)]
    else:
        pts = [(x, y), (x + tam, y - tam * 0.6), (x + tam, y + tam * 0.6)]
    d.polygon([(esc(a), esc(b)) for a, b in pts], fill=color)


def diodo(x, y, color=C_TINTA, etiqueta=None):
    t, a = 14, 12
    d.polygon([(esc(x - t), esc(y - a)), (esc(x - t), esc(y + a)), (esc(x + t), esc(y))],
              fill=color)
    d.line([(esc(x + t), esc(y - a)), (esc(x + t), esc(y + a))], fill=color, width=esc(4))
    if etiqueta:
        texto(x, y + 30, etiqueta, 13.5, C_SUAVE, False, "mm")


def fusible(x, y, color=C_TINTA, etiqueta=None):
    d.rounded_rectangle([esc(x - 20), esc(y - 11), esc(x + 20), esc(y + 11)], radius=esc(5),
                        outline=color, width=esc(2.6), fill=C_FONDO)
    cable([(x - 20, y), (x + 20, y)], color, 2.6)
    if etiqueta:
        texto(x, y - 26, etiqueta, 13.5, color, True, "mm")


# ── Titulo ───────────────────────────────────────────────────────────────────
texto(60, 40, "Excedente solar  →  frigo", 30, C_TINTA, True)
texto(60, 82, "NE185-11S · frigo AES · dos circuitos: la señal que le dice al frigo «hay 12 V» y la potencia que los entrega.", 16, C_SUAVE)

# ═════════════════════════════ 1. SEÑAL ═════════════════════════════════════
AX0, AY0, AX1, AY1 = 60, 120, 1740, 665
marco(AX0, AY0, AX1, AY1, "1 · SEÑAL  (D+ / S+)", C_SENAL,
      "Son señales: basta cable de ~1 mm². La entrada del frigo admite D+ (motor) o S+ (sol).")

YS = 350          # linea de señal
caja(AX0 + 50, YS - 60, AX0 + 330, YS + 60, "NE185 · J4", "salida D+  (0,5 A)", borde=(120, 140, 160))
cable([(AX0 + 330, YS), (1240, YS)], C_SENAL, 3.4)
texto(AX0 + 360, YS - 26, "con el motor en marcha", 14.5, C_SENAL, True, "lm")

caja(AX0 + 50, YS + 130, AX0 + 250, YS + 240, "P4", "GPIO1 · JP1 pin 7", borde=(210, 150, 90))
cable([(AX0 + 250, YS + 185), (AX0 + 310, YS + 185)], C_MANDO, 3.0)
flecha(AX0 + 306, YS + 185, "derecha", C_MANDO)
caja(AX0 + 310, YS + 130, AX0 + 570, YS + 240, "RELÉ PILOTO", "contacto NA", borde=(210, 150, 90))
cable([(AX0 + 570, YS + 185), (700, YS + 185)], C_MANDO, 3.0)
fusible(740, YS + 185, C_MANDO, "5 A")
cable([(780, YS + 185), (830, YS + 185)], C_MANDO, 3.0)
diodo(860, YS + 185, C_TINTA, "diodo: el anillo mira al frigo")
cable([(890, YS + 185), (1240, YS + 185), (1240, YS)], C_MANDO, 3.0)
texto(AX0 + 50, 616, "cuando sobra sol (modo del P4)", 14.5, C_MANDO, True, "la")
nodo(1240, YS, C_TINTA, 5.4)

caja(1240, YS - 60, AX1 - 40, YS + 60, "FRIGO", "entrada D+/S+", borde=(210, 150, 90))
texto(AX0 + 50, YS + 72, "El diodo impide que el P4 meta corriente en la salida del NE185, y que la D+ del alternador entre al piloto.", 14.5, C_TINTA)

# ═════════════════════════════ 2. POTENCIA ═════════════════════════════════
# Ya esta montada en la furgo: aqui solo se deja dicho QUE tiene que cumplir, sin
# dibujar trabajo que no hay que hacer.
BX0, BY0, BX1, BY1 = 60, 705, 1740, 1060
marco(BX0, BY0, BX1, BY1, "2 · POTENCIA  (12 V · 8-11 A)   —   ya montada", C_SERV,
      "No hay nada que hacer aquí: el frigo ya recibe sus 12 V. Solo tiene que cumplir esto.")

caja(BX0 + 50, BY0 + 120, BX0 + 620, BY0 + 300,
     "Con el motor", "los 12 V vienen del vehículo, como de serie (JP4-2 · F2 20 A)",
     borde=(120, 140, 160), sub_px=13.5)
caja(BX0 + 660, BY0 + 120, BX0 + 1230, BY0 + 300,
     "Con sol", "tienen que venir de la batería de servicios (ya conmutado)",
     borde=(198, 40, 40), sub_px=13.5)
texto(BX0 + 50, BY0 + 322, "Comprobación en un minuto: sin contacto y sin sol, mira de qué batería salen los 12 V del cable gordo del frigo.",
      14.5, C_TINTA)
texto(BX0 + 50, BY0 + 344, "Si salen de la de arranque, la señal S+ no puede activarse sin conmutar antes: el frigo tiraría 8-11 A de ella.",
      14.5, C_TINTA)

# ═════════════════════════════ Pie ══════════════════════════════════════════
FX0, FY0, FX1, FY1 = 60, 1235, 880, 1460
d.rounded_rectangle([esc(FX0), esc(FY0), esc(FX1), esc(FY1)], radius=esc(14),
                    fill=C_AVISO, outline=C_AVISO_B, width=esc(2))
texto(FX0 + 26, FY0 + 22, "Qué pasa en cada caso", 17, C_TINTA, True)
filas = [
    ("Con el motor", "el NE185 da su D+ (J4) y sus 12 V de serie (JP4-2)", C_VEH),
    ("Con sol (modo del P4)", "el P4 da S+ y conmuta la potencia a servicios", C_SERV),
    ("Aparcado y sin sol", "ni señal ni 12 V: el frigo se queda en gas", C_SUAVE),
    ("Aparece 230 V con sol", "el P4 suelta y vuelve todo a lo del NE185", C_TINTA),
]
for i, (a, b, col) in enumerate(filas):
    y = FY0 + 66 + i * 40
    d.ellipse([esc(FX0 + 28), esc(y - 5), esc(FX0 + 38), esc(y + 5)], fill=col)
    texto(FX0 + 52, y, a, 14.5, C_TINTA, True, "lm")
    texto(FX0 + 300, y, b, 13.5, C_SUAVE, False, "lm")

GX0, GY0, GX1, GY1 = 920, 1235, 1740, 1460
d.rounded_rectangle([esc(GX0), esc(GY0), esc(GX1), esc(GY1)], radius=esc(14),
                    outline=C_BORDE, width=esc(2.4), fill=C_CAJA)
texto(GX0 + 26, GY0 + 22, "Recuerda", 17, C_TINTA, True)
avisos = [
    "• El frigo tiene que estar en la salida POR RELÉ (JP4-2).",
    "• El encendido de gas (JP4-3, F7) no se toca.",
    "• JP13 (entrada de D+ del NE185) no se toca: acopla baterías y apaga la luz exterior.",
    "• Fusible de 20 A cerca de la batería de servicios.",
]
for i, a in enumerate(avisos):
    texto(GX0 + 26, GY0 + 66 + i * 30, a, 13.5, C_TINTA)

img = img.resize((W, H), Image.LANCZOS)
salida = Path(__file__).resolve().parent.parent / "docs" / "esquema_excedente_solar.png"
img.save(salida)
img.save(salida.with_suffix(".pdf"), "PDF", resolution=150.0)
print("PNG:", salida)
print("PDF:", salida.with_suffix(".pdf"))
