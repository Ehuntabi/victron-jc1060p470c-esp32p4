#!/usr/bin/env python3
"""captura_pantallas.py — Baja capturas de pantalla de la P4 por HTTP.

El firmware ya trae el carrusel de capturas: /capturas (lista) y
/captura?n=<i> (una pantalla en BMP). Este guion las baja todas, las convierte a
PNG (para poder mirarlas) y dice el tamano de cada una.

Necesita estar en la red de la P4 (o sea, el PC conectado a su AP).

Uso:  python3 captura_pantallas.py [carpeta] [ip] [usuario] [clave]
"""
import os, subprocess, sys, urllib.request, base64, re

CARPETA = sys.argv[1] if len(sys.argv) > 1 else "/tmp/capturas_p4"
IP      = sys.argv[2] if len(sys.argv) > 2 else "192.168.4.1"
USUARIO = sys.argv[3] if len(sys.argv) > 3 else ""
CLAVE   = sys.argv[4] if len(sys.argv) > 4 else ""

os.makedirs(CARPETA, exist_ok=True)
cabeceras = {}
if USUARIO:
    tok = base64.b64encode(f"{USUARIO}:{CLAVE}".encode()).decode()
    cabeceras["Authorization"] = "Basic " + tok

def bajar(url, destino):
    req = urllib.request.Request(url, headers=cabeceras)
    with urllib.request.urlopen(req, timeout=20) as r:
        datos = r.read()
    with open(destino, "wb") as f:
        f.write(datos)
    return len(datos)

print(f"== capturas de {IP} -> {CARPETA} ==")
# La lista de pantallas: el carrusel usa los mismos nombres que el tour.
try:
    req = urllib.request.Request(f"http://{IP}/capturas", headers=cabeceras)
    with urllib.request.urlopen(req, timeout=15) as r:
        html = r.read().decode("utf8", "replace")
    print(f"  /capturas responde {len(html)} bytes")
except Exception as e:
    print(f"  ERROR pidiendo /capturas: {e}"); sys.exit(1)

bajadas = 0
for n in range(0, 24):                    # el tour tiene ~20 pantallas
    destino = os.path.join(CARPETA, f"pantalla_{n:02d}.bmp")
    try:
        kb = bajar(f"http://{IP}/captura?n={n}", destino)
    except Exception as e:
        if n == 0:
            print(f"  ERROR con la primera captura: {e}")
            sys.exit(1)
        break                          # se acabo la lista
    png = destino[:-4] + ".png"
    hecho = subprocess.run(["convert", destino, png], capture_output=True).returncode == 0
    print(f"  pantalla {n:2}: {kb:7} bytes" + ("  -> PNG" if hecho else ""))
    bajadas += 1

print(f"\n{bajadas} capturas en {CARPETA}")
if bajadas and not os.path.exists(os.path.join(CARPETA, "pantalla_00.png")):
    print("(sin ImageMagick: quedan en BMP; se pueden ver con 'eog' o convertir con PIL)")
