#!/usr/bin/env bash
# rearmar_simulador.sh — deja el puente emitiendo los 3 Victron simulados.
# HACE FALTA CADA VEZ QUE SE ABRE EL PUERTO SERIE DEL PUENTE (abrir el puerto lo
# reinicia y el simulador se pierde; la P4 se queda sin datos y TODAS las
# tarjetas enseñan "--").

#!/bin/bash
cd /home/jc/joint/victron/tools/puente_pruebas
export PUENTE_AP_CLAVE=58xc6xfknk PUENTE_PORTAL_CLAVE=wau4xcyj
python3 -u - <<'PY'
import json, sys, time
sys.path.insert(0,'.')
from puente import Puente
p = Puente(); p.sincronizar()
print("[1] puente reiniciado y consola lista", flush=True)
p.esperar_ok("wifista VictronConfig 58xc6xfknk", veces=2, espera=25)
print("[2] asociado al AP de la P4", flush=True)
devs=[d for d in json.load(open("/tmp/devices_victron.json")) if d.get("nombre")][:3]
orden=("sim ble "+" ".join(f"{d['mac']} {d['key_hex']}" for d in devs))[:250]
for c in (orden,"sim ritmo 50","sim caos on"):
    ok,_=p.esperar_ok(c, veces=3, espera=12)
    print(f"[3] {c[:24]:24} {ok}", flush=True)
print("[4] estado:", [l for l in p.cmd("sim", espera=10) if l.startswith("SIM")], flush=True)
p.cerrar(); print("[5] listo: el simulador queda emitiendo", flush=True)
PY
