#!/usr/bin/env python3
"""verifica_parser.py — Comprueba que el parser de la P4 descifra BIEN los seis
tipos de registro, con aritmetica en vez de a ojo.

El simulador emite en modo extremo tramas cuyo contenido se conoce byte a byte
(centinelas 0x7FFF/0xFFFF y maximos). Este guion calcula lo que la P4 DEBE
imprimir a partir de esos bytes y lo compara con lo que imprimio de verdad
(segun /tmp/banco_p4.log). Si algun campo estuviera mapeado al reves o con el
signo cambiado, aqui salta.

Uso:  python3 verifica_parser.py [log]
"""
import re, sys

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/banco_p4.log"
# El log viene del puerto serie: lleva \r\n (y a veces solo \r). Se normaliza
# antes de buscar, que es lo que hacia fallar la primera version de este guion.
txt = open(LOG, errors="replace").read().replace("\r\n", "\n").replace("\r", "\n")

# Lo que TIENE que salir, calculado de las tramas extremas del simulador:
ESPERADO = [
    ("SmartSolar Charger",
     r"Vbat=327\.67V Ibat=3276\.7A PV=65535W Yield=655\.35kWh Load=0\.0A",
     "12 B: V e I a 0x7FFF (centinela), rendimiento y PV a 0xFFFF, carga a 0x1FF (=NA -> 0)"),
    ("Battery Monitor",
     r"Vbat=327\.67V Ibat=-0\.001A SOC=102\.3% TTG=65535 min",
     "15 B: cola a bits 22+20+10 al maximo -> corriente -1 mA, SOC 1023 (=102,3 %)"),
    ("Inverter",
     r"Vbat=327\.67V AC=327\.67V Iac=204\.7A P=65535VA",
     "11 B: VCA 15 bits y IAC 11 bits al maximo dentro de la cola de 32"),
    ("DC/DC Converter",
     r"State=255 Error=0xFF Vin=655\.35V Vout=327\.67V OffReason=0xFFFFFFFF",
     "10 B: entrada 0xFFFF y salida 0x7FFF (centinelas distintos), off_reason lleno"),
    ("Smart Lithium",
     r"Flags=0xFFFFFFFF Err=0xFFFF Batt=40\.95V Temp=215C",
     "17 B: tension empaquetada en 12 bits (0xFFF=40,95 V), balanceo en 4, temp 0xFF-40=215 C"),
    ("Orion XS DC/DC",
     r"State=255 Err=0xFF Vin=655\.35V Iin=6553\.5A Vout=655\.35V Iout=6553\.5A",
     "14 B: los cuatro valores a 65535 (V/100 y A/10)"),
]

print("=" * 78)
print("VERIFICACION DEL PARSER DE LA P4 (tramas extremas, calculadas a mano)")
print("=" * 78)
fallos = 0
for nombre, patron, porque in ESPERADO:
    # coger la linea de valores que sigue al titulo del tipo
    # La linea de valores va justo detras del titulo del tipo: se busca el
    # titulo y se comprueba que lo esperado aparezca en las 3 lineas siguientes.
    ok, linea = False, "(no aparece el titulo)"
    for m in re.finditer(rf"=== {re.escape(nombre)} ===\n((?:[^\n]*\n){{0,3}})", txt):
        trozo = m.group(1)
        linea = trozo.strip().splitlines()[0][:110] if trozo.strip() else "(vacio)"
        if re.search(patron, trozo):
            ok = True
            break
    if not ok: fallos += 1
    print(f"\n  {nombre}")
    print(f"    esperado: {patron}")
    print(f"    obtenido: {linea[:110]}")
    print(f"    {'OK' if ok else 'NO CUADRA'}   ({porque})")

print()
print("=" * 78)
print(f"{len(ESPERADO)-fallos} de {len(ESPERADO)} tipos descifran exactamente lo esperado"
      + ("" if fallos == 0 else f"  --  {fallos} REVISAR"))
