#!/usr/bin/env python3
"""Driver del puente de pruebas: manda ordenes y devuelve la respuesta.

Al abrir el puerto serie la placa se reinicia (cosa del USB-Serial-JTAG), asi
que primero se espera al aviso "puente>" y luego se manda la orden. La respuesta
se lee hasta la linea "OK" o "ERR ...".
"""
import serial, sys, time, glob, re

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

    def sincronizar(self, segundos=2.0):
        """Deja la consola en blanco: al abrir el puerto la placa se reinicia y
        el banner de arranque ('Type help...', 'Returned from app_main') se
        cuela en la respuesta de la PRIMERA orden, que entonces parece fallar."""
        self.p.reset_input_buffer()
        self.p.write(b"\r\n")
        t0 = time.time()
        while time.time() - t0 < segundos:
            self.p.read(4096)
        self.p.reset_input_buffer()

    def esperar_ok(self, orden, veces=3, espera=15.0, pausa=2.0):
        """Manda una orden y reintenta hasta ver OK. Devuelve (ok, salida)."""
        salida = []
        for intento in range(veces):
            salida = self.cmd(orden, espera=espera)
            if any(l == "OK" or l.startswith("OK ") for l in salida):
                return True, salida
            if intento + 1 < veces:
                time.sleep(pausa)
        return False, salida

    def asociar(self, ssid, ap_clave, usuario, portal_clave, segundos=25.0):
        """Deja el puente conectado como estacion al AP de la P4 y devuelve su
        IP (o None). La orden del puente es:
            sat <ssid> <claveAP> <usuario> <clavePortal>
        Sin esto, httpget/httppost no salen a ninguna parte."""
        ok, salida = self.esperar_ok(f"sat {ssid} {ap_clave} {usuario} {portal_clave}",
                                     veces=2, espera=segundos)
        if not ok:
            return None, salida
        # La IP tarda unos segundos en llegar (asociacion + DHCP): se pregunta
        # por los dos caminos, 'wifiip' y el informe del satelite.
        t0 = time.time()
        while time.time() - t0 < 40:
            for l in self.cmd("wifiip", espera=8.0):
                if l.startswith("IP "):
                    ip = l.split(None, 1)[1].strip()
                    if ip and ip != "(sin IP)":
                        return ip, salida
            for l in self.cmd("sat informe", espera=8.0):
                m = re.search(r"IP=(\d+\.\d+\.\d+\.\d+)", l)
                if m:
                    return m.group(1), salida
            time.sleep(2)
        return None, salida

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
