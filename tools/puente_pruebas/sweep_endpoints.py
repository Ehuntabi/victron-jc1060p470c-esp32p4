#!/usr/bin/env python3
"""sweep_endpoints.py — Barrido COMPLETO de los endpoints de la P4 desde el puente.

Usa el puente (ESP32) como cliente HTTP, asi que no hace falta que el PC este en
la red de la P4. Clasifica cada endpoint y comprueba que responde lo que toca:

  - los GET de lectura deben dar 200 (o 401/503 con motivo, y se apunta cual);
  - los POST se prueban con un cuerpo INVALIDO a proposito: tienen que dar 400
    (nunca 200), porque un cuerpo bueno cambiaria configuracion o abriria un viaje;
  - /settime se prueba con la hora actual, que es lo que hace la app.

Uso:  PUENTE_PORTAL_CLAVE=... python3 sweep_endpoints.py [usuario]
"""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from puente import Puente

USUARIO = sys.argv[1] if len(sys.argv) > 1 else "victron"
CLAVE   = os.environ.get("PUENTE_PORTAL_CLAVE", "")
if not CLAVE:
    print("ERROR: falta PUENTE_PORTAL_CLAVE"); sys.exit(1)

BASE, PESADO = "http://192.168.4.1", "http://192.168.4.1:8081"

# (endpoint, metodo, que se espera, si es del servidor pesado)
PRUEBAS = [
    ("/",                        "GET",  [200, 401], False),
    ("/api/state",               "GET",  [200],      False),
    ("/dashboard",               "GET",  [200, 401], False),
    ("/keys",                    "GET",  [200, 401], False),
    ("/capturas",                "GET",  [200, 401], False),
    ("/captura?n=1",             "GET",  [200, 401, 503], False),
    ("/snapshot",                "GET",  [200, 503], False),   # 503 = sin camara, correcto
    ("/ausente",                 "GET",  [200, 401], False),
    ("/vigilancia",              "GET",  [200, 401, 404], False),
    ("/generate_204",            "GET",  [204, 200, 302], False),
    ("/hotspot-detect.html",     "GET",  [200, 302, 301], False),
    ("/ncsi.txt",                "GET",  [200, 302, 301], False),
    ("/ota",                     "GET",  [200, 401], True),
    ("/data",                    "GET",  [200, 401], True),
    ("/data/bateria",            "GET",  [200, 401], True),
    ("/data/frigo",              "GET",  [200, 401], True),
    ("/data/viajes",             "GET",  [200, 401], True),
    ("/data/bateria.csv",        "GET",  [200, 401, 404], True),
    ("/data/frigo.csv",          "GET",  [200, 401], True),
    ("/data/ne185v.csv",         "GET",  [200, 401], True),
    ("/data/config.tar",         "GET",  [200, 401, 404], True),
    ("/data/logs.tar",           "GET",  [200, 401, 404], True),
    ("/api/alarma",              "POST", [400],      False),   # cuerpo invalido a proposito
    ("/api/viaje",               "POST", [400],      False),
    ("/save",                    "POST", [400],      False),
    ("/control",                 "POST", [400, 401], False),
]

def main():
    p = Puente()
    p.sincronizar()
    print(f"barrido de {len(PRUEBAS)} endpoints (puente en {p.puerto})\n")

    # El puente se reinicia al abrir el puerto y arranca SIN red: hay que
    # asociarlo al AP de la P4 antes de nada (si no, todo sale "?").
    ssid    = os.environ.get("PUENTE_AP_SSID", "VictronConfig")
    apclave = os.environ.get("PUENTE_AP_CLAVE", "")
    if apclave:
        ip, _ = p.asociar(ssid, apclave, USUARIO, CLAVE)
        if not ip:
            print("  !! el puente NO se ha asociado: no se pueden pedir endpoints\n")
            p.cerrar(); return 1
        print(f"  puente dentro de la red de la P4 (IP={ip})\n")
    else:
        print("  (sin PUENTE_AP_CLAVE: se supone que el puente ya estaba asociado)\n")

    print(f"  {'endpoint':28} {'met':4} {'esperado':12} {'obtenido':8} veredicto")
    fallos = []
    for ruta, metodo, esperados, pesado in PRUEBAS:
        url = (PESADO if pesado else BASE) + ruta
        if metodo == "POST":
            cuerpo = '{"prueba_invalida":1}' if ruta != "/settime" else \
                     json.dumps({"epoch": int(time.time())})
            orden = f"httppost {url} {USUARIO} {CLAVE} '{cuerpo}'"
        else:
            orden = f"httpget {url} {USUARIO} {CLAVE}"
        codigo = "?"
        for intento in (1, 2):          # la primera orden tras arrancar se pierde a veces
            salida = p.cmd(orden, espera=20)
            for l in salida:
                if l.startswith("HTTP "):
                    codigo = l.split()[1]
            if codigo != "?" or intento == 2:
                break
            time.sleep(2)
        ok = codigo.isdigit() and int(codigo) in esperados
        if not ok:
            fallos.append((ruta, metodo, esperados, codigo, salida[-2:]))
        print(f"  {ruta:28} {metodo:4} {str(esperados):12} {codigo:8} {'ok' if ok else 'REVISAR'}")
    p.cerrar()
    print()
    if fallos:
        print(f"== {len(fallos)} endpoints que no han dado lo esperado ==")
        for ruta, metodo, esp, got, cola in fallos:
            print(f"  {metodo} {ruta}: esperaba {esp}, dio {got}   {cola}")
    else:
        print("== todos los endpoints responden lo que toca ==")
    return 0

if __name__ == "__main__":
    sys.exit(main())
