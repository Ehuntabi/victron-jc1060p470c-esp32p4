#!/usr/bin/env python3
"""banco.py — Banco de pruebas de las DOS placas, a la vez.

  - P4: se abre su puerto UNA vez y se lee en un hilo (abrir el puerto la
    reinicia, asi que no se vuelve a tocar).
  - Puente: UN SOLO dueño del puerto. Un hilo lee sin parar (y guarda el log y
    cuenta reinicios); las ordenes se mandan con un cerrojo y se espera su OK.

Cuenta lo que se envia y lo que se recibe, y se re-arma solo si el puente se
reinicia. Todo queda en /tmp/banco_*.log y el resumen en /tmp/banco_informe.txt.

Uso:  PUENTE_AP_CLAVE=... PUENTE_PORTAL_CLAVE=... python3 banco.py <horas> [periodo_s]
"""
import collections, glob, json, os, queue, re, serial, sys, threading, time

DURACION_H = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
PERIODO    = int(sys.argv[2]) if len(sys.argv) > 2 else 300
AP_CLAVE     = os.environ.get("PUENTE_AP_CLAVE", "")
PORTAL_CLAVE = os.environ.get("PUENTE_PORTAL_CLAVE", "")
if not AP_CLAVE:
    print("ERROR: falta PUENTE_AP_CLAVE", flush=True); sys.exit(1)

FIN = time.time() + DURACION_H * 3600
LOG_P4, LOG_ESP, LOG_INF = "/tmp/banco_p4.log", "/tmp/banco_esp.log", "/tmp/banco_informe.txt"

estado = {
    "p4_boots": 0, "p4_assert": 0, "p4_panic": 0, "p4_wdt": 0, "p4_heap": 0,
    "p4_stack": 0, "p4_errores": collections.Counter(),
    "tipos": collections.Counter(), "esp_boots": 0, "esp_primer_arranque": None,
    "resumenes": 0, "lineas_p4": 0, "lineas_esp": 0, "sat_lineas": 0,
}
lock = threading.Lock()


class Puente:
    """Dueño unico del puerto del puente: lee siempre y manda cuando toca."""
    def __init__(self, puerto):
        self.p = serial.Serial(puerto, 115200, timeout=0.3)
        self.p.setDTR(False); self.p.setRTS(False)
        self.cola = queue.Queue()
        self.log = open(LOG_ESP, "a", buffering=1)
        self.cmd_lock = threading.Lock()
        self.fin = False
        threading.Thread(target=self._lector, daemon=True).start()
        self._prompt()

    def _lector(self):
        while not self.fin and time.time() < FIN:
            try:
                d = self.p.read(4096)
            except Exception:
                time.sleep(0.5); continue
            if not d: continue
            txt = d.decode("utf8", "replace")
            self.log.write(txt)
            with lock:
                estado["lineas_esp"] += txt.count("\n")
                estado["esp_boots"] += txt.count("ESP-ROM:esp32c6")
                estado["resumenes"] += txt.count("RESUMEN")
                estado["sat_lineas"] += txt.count("SAT ")
            for l in txt.splitlines():
                l = l.strip()
                if l: self.cola.put(l)

    def _prompt(self, s=15):
        t0 = time.time()
        while time.time() - t0 < s:
            try:
                if "puente>" in self.cola.get(timeout=0.5):
                    return True
            except queue.Empty:
                pass
        return False

    def cmd(self, orden, espera=12):
        with self.cmd_lock:
            while not self.cola.empty():          # tirar lo viejo
                try: self.cola.get_nowait()
                except queue.Empty: break
            self.p.write((orden + "\n").encode())
            t0, salida = time.time(), []
            while time.time() - t0 < espera:
                try:
                    l = self.cola.get(timeout=0.4)
                except queue.Empty:
                    continue
                salida.append(l)
                if l == "OK" or l.startswith("ERR"):
                    return salida
            return salida

    def cerrar(self):
        self.fin = True
        try: self.log.close()
        except Exception: pass
        try: self.p.close()
        except Exception: pass


def lector_p4(puerto):
    f = open(LOG_P4, "a", buffering=1)
    p = serial.Serial(puerto, 115200, timeout=0.4)
    p.setDTR(False); p.setRTS(False)
    while time.time() < FIN:
        try:
            d = p.read(8192)
        except Exception:
            time.sleep(1); continue
        if not d: continue
        txt = d.decode("utf8", "replace")
        f.write(txt)
        with lock:
            estado["lineas_p4"] += txt.count("\n")
            estado["p4_boots"] += txt.count("ESP-ROM:esp32p4")
            estado["p4_assert"] += txt.count("assert failed")
            estado["p4_panic"] += txt.count("Guru Meditation")
            estado["p4_wdt"] += txt.count("task_wdt") + txt.count("Interrupt wdt")
            estado["p4_heap"] += txt.count("Not enough heap")
            estado["p4_stack"] += txt.count("stack overflow")
            for m in re.finditer(r"=== (SmartSolar Charger|Battery Monitor|Inverter|DC/DC Converter|Smart Lithium|Orion XS DC/DC) ===", txt):
                estado["tipos"][m.group(1)] += 1
            for ln in txt.splitlines():
                if ln.startswith("E (") and not any(x in ln for x in ("1-wire", "ne185", "swap_xy")):
                    estado["p4_errores"][ln.split(") ", 1)[-1][:60]] += 1
    f.close()
    p.close()


def clasifica(puerto, segundos=5):
    try:
        p = serial.Serial(puerto, 115200, timeout=0.3)
        p.setDTR(False); p.setRTS(False)
    except Exception:
        return None
    t0, txt = time.time(), ""
    while time.time() - t0 < segundos:
        try:
            d = p.read(4096)
            if d: txt += d.decode("utf8", "replace")
        except Exception:
            break
    p.close()
    if "PUENTE DE PRUEBAS" in txt or "puente>" in txt or "puente_pruebas" in txt: return "puente"
    if "VICTRON_LVGL_APP" in txt or "esp32p4" in txt: return "p4"
    return None


def main():
    puertos = sorted(glob.glob("/dev/ttyACM*"))
    print("puertos:", puertos, flush=True)
    if len(puertos) < 2:
        print("ERROR: hacen falta las dos placas", flush=True); return 1
    asign = {}
    for pt in puertos:
        q = clasifica(pt)
        if q and q not in asign: asign[q] = pt
    print("asignacion:", asign, flush=True)
    if "p4" not in asign or "puente" not in asign:
        print("ERROR: no distingo las placas", flush=True); return 1

    threading.Thread(target=lector_p4, args=(asign["p4"],), daemon=True).start()
    time.sleep(2)
    pte = Puente(asign["puente"])

    devs = [d for d in json.load(open("/tmp/devices_victron.json")) if d["nombre"]][:3]
    orden_ble = ("sim ble " + " ".join(f"{d['mac']} {d['key_hex']}" for d in devs))[:250]

    def mandar(orden, veces=5):
        for _ in range(veces):
            salida = pte.cmd(orden)
            ok = [l for l in salida if l.startswith("OK")]
            if ok:
                print("   ok:", ok[0][:110], flush=True); return True
            time.sleep(2)
        print("   !! no acepta:", orden[:70], flush=True); return False

    def armar():
        pte.cmd("")
        mandar(orden_ble)
        mandar("sim ritmo 50")
        mandar("sim caos on")
        mandar(f"sat VictronConfig {AP_CLAVE} victron {PORTAL_CLAVE}")

    armar()
    print(f"== BANCO EN MARCHA {DURACION_H} h (informe cada {PERIODO}s) ==", flush=True)
    inf = open(LOG_INF, "a", buffering=1)
    inf.write(f"\n===== banco {time.strftime('%Y-%m-%d %H:%M:%S')} durante {DURACION_H} h =====\n")
    with lock:
        boots_previos = estado["esp_boots"]

    while time.time() < FIN:
        time.sleep(PERIODO)
        with lock:
            t = {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in estado.items()}
            reiniciado = estado["esp_boots"] > boots_previos
            boots_previos = estado["esp_boots"]
        if reiniciado:
            print(f"!! el puente se ha reiniciado ({t['esp_boots']} veces): lo re-armo", flush=True)
            armar()
        sim = pte.cmd("sim")
        sat = pte.cmd("sat informe")
        linea = (f"[{time.strftime('%H:%M:%S')}] P4: arranques={t['p4_boots']} assert={t['p4_assert']} "
                 f"panic={t['p4_panic']} wdt={t['p4_wdt']} heap={t['p4_heap']} stack={t['p4_stack']} "
                 f"| descifrados={sum(t['tipos'].values())} {dict(t['tipos'])}")
        inf.write(linea + "\n")
        inf.write("   PUENTE: " + " | ".join(x for x in sim if x.startswith("SIM"))[:300] + "\n")
        inf.write("   SATELITE: " + " | ".join(x for x in sat if "SAT" in x or "HTTP" in x or "CRC" in x)[:400] + "\n")
        print(linea, flush=True)
        for x in sat:
            if x.startswith(("SAT", "   ")) and ("CRC" in x or "HTTP" in x or "UDP" in x or "IP=" in x):
                print("   ", x[:150], flush=True)

    pte.cerrar()
    with lock:
        t = {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in estado.items()}
    resumen = (f"\n===== FIN DEL BANCO =====\n"
               f"P4: {t['lineas_p4']} lineas | arranques={t['p4_boots']} | asserts={t['p4_assert']} | "
               f"panicos={t['p4_panic']} | wdt={t['p4_wdt']} | heap={t['p4_heap']} | stack={t['p4_stack']}\n"
               f"registros descifrados: {sum(t['tipos'].values())} {dict(t['tipos'])}\n"
               f"errores de la P4: {dict(collections.Counter(t['p4_errores']).most_common(10))}\n"
               f"puente: {t['lineas_esp']} lineas | arranques={t['esp_boots']} | resumenes={t['resumenes']} | "
               f"lineas de satelite={t['sat_lineas']}\n")
    inf.write(resumen); inf.close()
    print(resumen, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
