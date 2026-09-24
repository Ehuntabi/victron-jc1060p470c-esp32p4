#!/usr/bin/env python3
"""analiza.py — Informe del banco de pruebas a partir de lo que dejo banco.py.

Lee /tmp/banco_informe.txt (los informes periodicos) y los dos logs completos
(/tmp/banco_p4.log y /tmp/banco_esp.log) y saca un resumen: que se envio, que se
descifro, que se perdio, latencias, escrituras en la SD e incidencias.

Uso:  python3 analiza.py
"""
import collections, re, sys

INF, P4, ESP = "/tmp/banco_informe.txt", "/tmp/banco_p4.log", "/tmp/banco_esp.log"

def leer(p):
    try:
        return open(p, errors="replace").read()
    except FileNotFoundError:
        return ""

def seg(h):
    p = [int(x) for x in h.split(":")]
    return p[0] * 3600 + p[1] * 60 + p[2]

inf, p4, esp = leer(INF), leer(P4), leer(ESP)
if not inf:
    print("no hay informes todavia"); sys.exit(1)

marcas, desc, udp, http = [], [], [], []
marca = None
for ln in inf.splitlines():
    m = re.match(r"\[(\d+:\d+:\d+)\] P4: .*descifrados=(\d+)", ln)
    if m:
        marca = m.group(1); marcas.append(marca); desc.append(int(m.group(2)))
    m = re.search(r"telemetria UDP: (\d+) paquetes, (\d+) con CRC mal", ln)
    if m and marca: udp.append((marca, int(m.group(1)), int(m.group(2))))
    m = re.search(r"HTTP: (\d+) peticiones, (\d+) ok, (\d+) rechazadas\(4xx\), (\d+) sin respuesta", ln)
    if m and marca: http.append((marca,) + tuple(int(x) for x in m.groups()))

print("=" * 78)
print("INFORME DEL BANCO DE PRUEBAS (dos placas: P4 y puente ESP32)")
print("=" * 78)
if len(marcas) >= 2:
    dur = seg(marcas[-1]) - seg(marcas[0])
    print(f"duracion medida: {dur} s ({dur/60:.1f} min)  de {marcas[0]} a {marcas[-1]}")

# ── Enviado por el puente (ultimo RESUMEN) ──────────────────────────────────
env = None
for ln in esp.splitlines():
    m = re.search(r"RESUMEN ble=(\d+) solar=(\d+) bat=(\d+) inv=(\d+) dcdc=(\d+) litio=(\d+) orion=(\d+) modo=(\S+)", ln)
    if m: env = m.groups()
tipos = ["solar", "bat", "inv", "dcdc", "litio", "orion"]
etiquetas = {"solar": "SmartSolar Charger", "bat": "Battery Monitor", "inv": "Inverter",
             "dcdc": "DC/DC Converter", "litio": "Smart Lithium", "orion": "Orion XS DC/DC"}
if env:
    enviado = dict(zip(tipos, [int(x) for x in env[1:7]]))
    print(f"\nENVIADO por el puente: {int(env[0])} tramas  (modo final: {env[7]})")
    print(f"  {'tipo':22} {'enviado':>9} {'descifrado':>11} {'%':>6}")
    total_e = total_d = 0
    for k in tipos:
        d = p4.count(f"=== {etiquetas[k]} ===")
        total_e += enviado[k]; total_d += d
        print(f"  {etiquetas[k]:22} {enviado[k]:9} {d:11} {100.0*d/enviado[k] if enviado[k] else 0:5.1f}%")
    print(f"  {'TOTAL':22} {total_e:9} {total_d:11} {100.0*total_d/total_e if total_e else 0:5.1f}%")
    print("  (no todo lo enviado debe descifrarse: el modo caos manda a proposito")
    print("   tramas malformadas, y el escaner de la P4 no oye el 100% de los anuncios)")

# ── Telemetria UDP (la P4 manda 1 Hz) ───────────────────────────────────────
if len(udp) >= 2:
    (t0, u0, c0), (t1, u1, c1) = udp[0], udp[-1]
    dt = seg(t1) - seg(t0)
    print(f"\nTELEMETRIA UDP (la P4 emite a 1 Hz clavado, vTaskDelayUntil 1000 ms)")
    print(f"  recibidos {u1-u0} de ~{dt} esperados  ->  {100.0*(u1-u0)/dt if dt else 0:.1f} %")
    print(f"  CRC malos: {c1-c0}   (0 = ningun paquete roto)")
    print("  OJO: medido con el satelite ANTES de desactivar su ahorro de energia;")
    print("  una estacion con modem-sleep pierde difusiones entre balizas DTIM.")

# ── HTTP ────────────────────────────────────────────────────────────────────
if len(http) >= 2:
    a, b = http[0], http[-1]
    print(f"\nHTTP (satelite -> portal de la P4)")
    print(f"  {b[1]-a[1]} peticiones -> {b[2]-a[2]} ok, {b[3]-a[3]} rechazadas(4xx), {b[4]-a[4]} sin respuesta")
por_url = collections.Counter(re.findall(r"SAT (GET|POST) (\S+) -> (\d+)", esp))
if por_url:
    print("  por endpoint:")
    for (met, url, cod), n in sorted(por_url.items(), key=lambda x: -x[1])[:10]:
        print(f"    {n:4}x {met} {url[:42]:42} -> {cod}")

# ── Incidencias de la P4 ────────────────────────────────────────────────────
print(f"\nINCIDENCIAS DE LA P4")
for nombre, pat in [("reinicios (arranques)", "ESP-ROM:esp32p4"), ("asserts", "assert failed"),
                    ("panicos (Guru)", "Guru Meditation"), ("task watchdog", "task_wdt"),
                    ("interrupt watchdog", "Interrupt wdt"), ("stack overflow", "stack overflow"),
                    ("sin memoria (heap)", "Not enough heap")]:
    print(f"  {nombre:22} {p4.count(pat)}")
errores = collections.Counter(l.split(") ", 1)[-1][:55] for l in p4.splitlines()
                             if l.startswith("E (") and not any(x in l for x in ("1-wire", "ne185", "swap_xy")))
if errores:
    print("  errores (aparte de los del banco):")
    for k, v in errores.most_common(6):
        print(f"    {v:4}x {k}")

# ── Datos guardados en la SD (perdida de datos?) ────────────────────────────
volcados = len(re.findall(r"DATALOGGER: Volcadas", p4))
logsaves = len(re.findall(r"LOGSAVE: autosave OK", p4))
print(f"\nDATOS EN LA SD")
print(f"  volcados del datalogger: {volcados}")
print(f"  autosave del log:        {logsaves}")
print(f"  registros del frigo:     {len(re.findall(r'DATALOGGER: Log\\[', p4))}")

# ── Tendencia: se mantiene el ritmo o se degrada? ───────────────────────────
if len(desc) >= 3:
    print(f"\nTENDENCIA (registros descifrados por intervalo)")
    for i in range(1, len(desc)):
        dt = seg(marcas[i]) - seg(marcas[i-1])
        ritmo = (desc[i] - desc[i-1]) / dt if dt else 0
        print(f"  {marcas[i]}: +{desc[i]-desc[i-1]:5}  ({ritmo:.1f}/s)")
print("=" * 78)
