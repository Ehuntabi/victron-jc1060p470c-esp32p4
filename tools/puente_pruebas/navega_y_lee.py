#!/usr/bin/env python3
"""navega_y_lee.py — Navega la P4 a una pantalla por HTTP y lee su log a la vez.

Para que sirve: las paginas de Ajustes se construyen de forma PEREZOSA (la
primera vez que se entra), asi que si quieres ver un log de construccion -- o
comprobar una medida de layout -- hay que entrar en la pantalla de verdad. Este
guion lo hace sin tocar la pantalla fisica: pide /captura?n=<i>, que ademas de
devolver el BMP NAVEGA la pantalla; y mientras, lee el puerto serie de la P4
para capturar las lineas que te interesan.

Indices del tour (los mismos nombres que el carrusel de capturas, ver
capture_carousel.c): 14 frigo · 15 logs · 16 wifi · 17 display · 18 tarjeta_sd ·
19 sonido · 20 autocaravana · 21 victron_keys · 22 gps · 23 about.

Uso:
  PUENTE_AP_CLAVE=... PUENTE_PORTAL_CLAVE=... python3 navega_y_lee.py 20 [texto]
      [texto] = filtro de las lineas del log (por defecto "UI_SETTINGS")

Requiere: el puente (ESP32-C6) en /dev/ttyACM1 con la WiFi asociada, y la P4 en
/dev/ttyACM0. Abrir el puerto de la P4 la reinicia, asi que primero se deja
arrancar y luego se pide la pantalla.
"""
import os, re, serial, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from puente import Puente

INDICE  = sys.argv[1] if len(sys.argv) > 1 else "20"
FILTRO  = sys.argv[2] if len(sys.argv) > 2 else "UI_SETTINGS"
AP_CLAVE = os.environ.get("PUENTE_AP_CLAVE", "")
CLAVE    = os.environ.get("PUENTE_PORTAL_CLAVE", "")
USUARIO  = os.environ.get("PUENTE_PORTAL_USUARIO", "victron")
SSID     = os.environ.get("PUENTE_AP_SSID", "VictronConfig")
PUERTO_P4 = os.environ.get("PUERTO_P4", "/dev/ttyACM0")

if not AP_CLAVE or not CLAVE:
    print("ERROR: faltan PUENTE_AP_CLAVE y/o PUENTE_PORTAL_CLAVE"); sys.exit(1)

lineas = []


def lector():
    """Lee la P4 tolerando que el puerto se re-enumere (pasa al grabar)."""
    t0 = time.time()
    while time.time() - t0 < 150:
        try:
            sp = serial.Serial(PUERTO_P4, 115200, timeout=0.5)
            sp.setDTR(False); sp.setRTS(False)
            while time.time() - t0 < 150:
                d = sp.read(65536)
                if d:
                    for l in d.decode("utf8", "replace").splitlines():
                        if FILTRO in l or "assert" in l or "Guru" in l:
                            lineas.append(l.strip())
            sp.close()
        except Exception:
            time.sleep(1)     # el puerto se fue (re-enumeracion): reintentar


def main():
    threading.Thread(target=lector, daemon=True).start()
    time.sleep(20)            # dejar arrancar la P4 (abrir el puerto la reinicia)
    p = Puente(); p.sincronizar()
    p.esperar_ok(f"wifista {SSID} {AP_CLAVE}", veces=2, espera=25)
    print("[puente asociado]", flush=True)
    for intento in (1, 2, 3):
        sal = p.cmd(f"httpget http://192.168.4.1/captura?n={INDICE} {USUARIO} {CLAVE}",
                    espera=40)
        cod = [l for l in sal if l.startswith("HTTP")]
        print(f"  intento {intento}: {cod[0] if cod else sal[-1][:60]}", flush=True)
        if cod and "200" in cod[0]:
            break
        time.sleep(3)
    p.cerrar()
    time.sleep(2)
    print(f"--- lineas del log con '{FILTRO}' ---")
    for l in lineas:
        print("   ", l[:180])
    if not lineas:
        print("   (ninguna: ¿se abrio la pantalla? ¿el filtro es el correcto?)")


if __name__ == "__main__":
    main()
