#!/usr/bin/env python3
"""Driver del puente de pruebas: manda ordenes y devuelve la respuesta.

Al abrir el puerto serie la placa se reinicia (cosa del USB-Serial-JTAG), asi
que primero se espera al aviso "puente>" y luego se manda la orden. La respuesta
se lee hasta la linea "OK" o "ERR ...".
"""
import serial, sys, time, glob

PROMPT = "puente>"

class Puente:
    def __init__(self, puerto=None):
        if puerto is None:
            puerto = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))[-1]
        self.puerto = puerto
        self.p = serial.Serial(puerto, 115200, timeout=0.3)
        self.p.setDTR(False); self.p.setRTS(False)
        self._esperar_prompt(12)

    def _esperar_prompt(self, segundos):
        t0 = time.time()
        cola = b""
        while time.time() - t0 < segundos:
            try:
                d = self.p.read(4096)
            except Exception:
                break
            if d:
                cola += d
                if PROMPT.encode() in cola:
                    return True
        return False

    def cmd(self, orden, espera=15.0):
        self.p.reset_input_buffer()
        self.p.write((orden + "\n").encode())
        t0, lineas = time.time(), []
        while time.time() - t0 < espera:
            try:
                d = self.p.read(4096)
            except Exception:
                break
            if d:
                for l in d.decode("utf8", "replace").splitlines():
                    l = l.strip()
                    if not l or l == PROMPT or l.endswith("[5n"):
                        continue
                    lineas.append(l)
                    if l == "OK" or l.startswith("ERR"):
                        return lineas
        return lineas

    def cerrar(self):
        self.p.close()

if __name__ == "__main__":
    p = Puente()
    print(f"(puente en {p.puerto})")
    for orden in (sys.argv[1:] or ["status"]):
        print(f"--- {orden}")
        for l in p.cmd(orden):
            print("   ", l[:170])
    p.cerrar()
