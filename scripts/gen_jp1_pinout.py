#!/usr/bin/env python3
"""Regenera docs/jp1_pinout.png del 7" Guition JC1060P470C_I.

Uso:
    python3 scripts/gen_jp1_pinout.py       # escribe docs/jp1_pinout.png

Si modificas la tabla PINS de abajo, ejecuta este script y luego
re-exporta el PDF con chromium para que la imagen embebida quede al día:

    chromium --headless --disable-gpu --no-pdf-header-footer \\
        --print-to-pdf=docs/pinout_guition_jc1060p470c_i.pdf \\
        file://$(pwd)/docs/pinout_guition_jc1060p470c_i.html
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

# Colores (de la leyenda original)
C_5V    = (212, 47, 47)      # rojo
C_3V3   = (245, 124, 0)      # naranja
C_GND   = (38, 38, 38)       # negro
C_FREE  = (56, 142, 60)      # verde GPIO libre
C_USE   = (251, 192, 45)     # amarillo GPIO en uso
C_I2C   = (180, 215, 240)    # azul claro I2C externo
C_C6    = (160, 160, 160)    # gris ESP32-C6
C_NC    = (215, 215, 215)    # gris claro NC

C_TXT_DARK  = (0, 0, 0)
C_TXT_WHITE = (255, 255, 255)
C_BG        = (255, 255, 255)

# Pin data: (color, nombre, subtitulo, color_texto)
PINS = {
    1:  (C_3V3, "VCC3V3", "", C_TXT_WHITE),
    2:  (C_5V,  "VOUT-BAT", "", C_TXT_WHITE),
    3:  (C_3V3, "VCC3V3", "", C_TXT_WHITE),         # FIX: era NC
    4:  (C_5V,  "VOUT-BAT", "", C_TXT_WHITE),       # FIX: era NC
    5:  (C_GND, "GND", "", C_TXT_WHITE),
    6:  (C_GND, "GND", "", C_TXT_WHITE),
    7:  (C_USE, "GPIO 1", "PZEM UART2 TX", C_TXT_DARK),
    8:  (C_NC,  "NC", "no conectado", C_TXT_DARK),
    9:  (C_USE, "GPIO 2", "PZEM UART2 RX", C_TXT_DARK),
    10: (C_USE, "GPIO 47 Touch", "EN USO — Touch interno", C_TXT_DARK),
    11: (C_FREE, "GPIO 3", "", C_TXT_WHITE),
    12: (C_FREE, "GPIO 46", "", C_TXT_WHITE),
    13: (C_USE, "GPIO 4", "DS18B20 Frigo", C_TXT_DARK),
    14: (C_FREE, "GPIO 45", "", C_TXT_WHITE),
    15: (C_USE, "GPIO 5", "Fan PWM Frigo", C_TXT_DARK),
    16: (C_GND, "GND", "", C_TXT_WHITE),
    17: (C_FREE, "GPIO 20 ADC", "", C_TXT_WHITE),
    18: (C_3V3, "VCC3V3", "", C_TXT_WHITE),
    19: (C_FREE, "GPIO 32", "", C_TXT_WHITE),
    20: (C_C6,  "C6 U0RXD", "EN USO — radio ESP32-C6", C_TXT_DARK),
    21: (C_FREE, "GPIO 33", "", C_TXT_WHITE),
    22: (C_C6,  "C6 U0TXD", "EN USO — radio ESP32-C6", C_TXT_DARK),
    23: (C_I2C, "ES I2C SDA", "EN USO — bus I²C interno", C_TXT_DARK),
    24: (C_C6,  "C6 IO9", "EN USO — radio ESP32-C6", C_TXT_DARK),
    25: (C_I2C, "ES I2C SCL", "EN USO — bus I²C interno", C_TXT_DARK),
    26: (C_C6,  "C6 CHIP PU", "EN USO — radio ESP32-C6", C_TXT_DARK),
}

# Layout
IMG_W = 750
MARGIN = 30
CARD_W = 320
CARD_H = 84
CARD_GAP_X = 10  # espacio centro entre las 2 columnas
CARD_GAP_Y = 10
RADIUS = 12

HDR_H = 70
LEG_H = 100
N_ROWS = 13
IMG_H = HDR_H + MARGIN + N_ROWS * (CARD_H + CARD_GAP_Y) + LEG_H + MARGIN

# Fuentes (DejaVu suele estar en /usr/share/fonts/truetype/dejavu/)
def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    paths = [
        f"/usr/share/fonts/truetype/dejavu/{name}",
        f"/usr/share/fonts/dejavu/{name}",
    ]
    for p in paths:
        if Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

F_TITLE = font(34, bold=True)
F_NAME  = font(24, bold=True)
F_SUB   = font(20)
F_PIN   = font(22, bold=True)
F_LEG   = font(13)

img = Image.new("RGB", (IMG_W, IMG_H), C_BG)
d = ImageDraw.Draw(img)

# Título
title = "JP1 — Pin Header 2×13 / 2.54 mm"
bbox = d.textbbox((0, 0), title, font=F_TITLE)
tw = bbox[2] - bbox[0]
d.text(((IMG_W - tw) // 2, 15), title, font=F_TITLE, fill=C_TXT_DARK)

# Cards
col_left_x  = MARGIN
col_right_x = IMG_W - MARGIN - CARD_W

def draw_card(x, y, pin, data, pin_align_right):
    color, name, sub, tcol = data
    d.rounded_rectangle((x, y, x + CARD_W, y + CARD_H), radius=RADIUS, fill=color)
    # Número de pin en esquina
    pin_str = str(pin)
    pbox = d.textbbox((0, 0), pin_str, font=F_PIN)
    pw = pbox[2] - pbox[0]
    if pin_align_right:
        px = x + CARD_W - 10 - pw
    else:
        px = x + 10
    d.text((px, y + 6), pin_str, font=F_PIN, fill=tcol)
    # Nombre centrado
    nbox = d.textbbox((0, 0), name, font=F_NAME)
    nw = nbox[2] - nbox[0]
    nh = nbox[3] - nbox[1]
    name_y = y + (CARD_H - nh) // 2 - (14 if sub else 0)
    d.text((x + (CARD_W - nw) // 2, name_y), name, font=F_NAME, fill=tcol)
    # Subtítulo centrado debajo
    if sub:
        sbox = d.textbbox((0, 0), sub, font=F_SUB)
        sw = sbox[2] - sbox[0]
        d.text((x + (CARD_W - sw) // 2, name_y + nh + 10), sub,
               font=F_SUB, fill=tcol)

y = HDR_H + MARGIN
for row in range(N_ROWS):
    pin_l = 2 * row + 1
    pin_r = 2 * row + 2
    draw_card(col_left_x,  y, pin_l, PINS[pin_l], pin_align_right=True)
    draw_card(col_right_x, y, pin_r, PINS[pin_r], pin_align_right=False)
    y += CARD_H + CARD_GAP_Y

# Leyenda inferior — 2 filas × 4 columnas
leg_y = y + 10
leg_items = [
    (C_5V,   "5 V",          C_TXT_WHITE),
    (C_3V3,  "3.3 V",        C_TXT_WHITE),
    (C_GND,  "GND",          C_TXT_WHITE),
    (C_FREE, "GPIO libre",   C_TXT_WHITE),
    (C_USE,  "GPIO en uso",  C_TXT_DARK),
    (C_I2C,  "I²C externo",  C_TXT_DARK),
    (C_C6,   "ESP32-C6",     C_TXT_DARK),
    (C_NC,   "NC / sin conexión", C_TXT_DARK),
]
SQ = 22
COL_W = (IMG_W - 2 * MARGIN) // 4
for i, (c, label, _tc) in enumerate(leg_items):
    col = i % 4
    row = i // 4
    x = MARGIN + col * COL_W
    yy = leg_y + row * (SQ + 12)
    d.rounded_rectangle((x, yy, x + SQ, yy + SQ), radius=4, fill=c)
    d.text((x + SQ + 8, yy + 3), label, font=F_LEG, fill=C_TXT_DARK)

out = Path(__file__).resolve().parent.parent / "docs" / "jp1_pinout.png"
img.save(out, "PNG", optimize=True)
print(f"OK -> {out} ({out.stat().st_size} bytes)")
