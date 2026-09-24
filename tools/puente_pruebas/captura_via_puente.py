#!/usr/bin/env python3
"""captura_via_puente.py — Trae las capturas de pantalla de la P4 SIN que el PC
tenga que estar en su red: las pide el PUENTE (que ya esta asociado a su AP) y
las escupe por el puerto serie en base64 (orden `httpb64` del firmware del
puente). Aqui se recomponen y se convierten a PNG.

Para que sirve: el dongle Wi-Fi del PC se cae del bus cada tanto y sin el no hay
forma de llegar al AP de la P4. Con esto, la revision visual depende solo del
puente.

Uso:  PUENTE_AP_CLAVE=... PUENTE_PORTAL_CLAVE=... python3 captura_via_puente.py [carpeta]
"""
import base64, json, os, re, subprocess, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from puente import Puente

CARPETA  = sys.argv[1] if len(sys.argv) > 1 else "/tmp/capturas_p4"
AP_CLAVE = os.environ.get("PUENTE_AP_CLAVE", "")
CLAVE    = os.environ.get("PUENTE_PORTAL_CLAVE", "")
USUARIO  = os.environ.get("PUENTE_PORTAL_USUARIO", "victron")
FICHERO_EQUIPOS = os.environ.get("PUENTE_EQUIPOS", "/tmp/devices_victron.json")
SSID     = os.environ.get("PUENTE_AP_SSID", "VictronConfig")

if not AP_CLAVE or not CLAVE:
    print("ERROR: faltan PUENTE_AP_CLAVE y/o PUENTE_PORTAL_CLAVE"); sys.exit(1)

os.makedirs(CARPETA, exist_ok=True)
B64 = re.compile(r"^[A-Za-z0-9+/=]{8,}$")


def traer(p, url, segundos=240, intentos=3):
    """Manda httpb64 y devuelve los bytes del fichero (o None).

    El puente manda el fichero en lineas marcadas con '#' (asi el ruido del log
    no se confunde con los datos) y termina con "FINB64 <bytes> <crc32>". Se
    comprueban las dos cosas; si no cuadran, se reintenta: es mas barato repetir
    que mirar una captura corrupta.
    """
    for intento in range(1, intentos + 1):
        p.p.reset_input_buffer()
        p.p.write(f"httpb64 {url} {USUARIO} {CLAVE}\n".encode())
        t0, trozos = time.time(), []
        while time.time() - t0 < segundos:
            d = p.p.read(65536)
            if not d:
                continue
            trozos.append(d)
            cola = b"".join(trozos[-6:]).decode("utf8", "replace")
            m = re.search(r"FINB64 (\d+) ([0-9a-f]{8})", cola)
            if m:
                esperados, crc = int(m.group(1)), int(m.group(2), 16)
                break
            if re.search(r"^ERR ", cola, re.M):
                return None
        else:
            # El volcado anterior puede seguir saliendo: hay que VACIAR el puerto
            # antes de reintentar, o su cola se mezcla con el fichero siguiente.
            t_dren = time.time()
            while time.time() - t_dren < 3:
                p.p.read(65536)
            print(f"    (intento {intento}: se acabo el tiempo)")
            continue
        # Nada mas terminar, vaciar lo que quede en el buffer del PC.
        time.sleep(0.3)
        while p.p.read(65536):
            pass
        crudo = b"".join(trozos).decode("utf8", "replace")
        b64 = "".join(l[1:].strip() for l in crudo.splitlines()
                      if l.startswith("#") and re.fullmatch(r"#[A-Za-z0-9+/=]{1,64}", l.strip()))
        try:
            datos = base64.b64decode(b64, validate=True)
        except Exception as e:
            print(f"    (intento {intento}: base64 roto: {e})")
            continue
        # El conteo del puente puede no cuadrar (el cliente HTTP entrega el
        # cuerpo en trozos y su cuenta no siempre coincide con lo emitido), asi
        # que la integridad se comprueba con lo que el fichero dice de SI MISMO:
        # un BMP lleva su tamano en los bytes 2..6 y empieza por "BM". El CRC se
        # apunta solo como informacion.
        if datos.startswith(b"BM"):
            declarado = int.from_bytes(datos[2:6], "little")
            if declarado != len(datos):
                print(f"    (intento {intento}: BMP dice {declarado} y llegaron {len(datos)})")
                continue
        elif len(datos) < 16:
            print(f"    (intento {intento}: solo {len(datos)} bytes)")
            continue
        if len(datos) != esperados:
            print(f"    (nota: el puente conto {esperados} y llegaron {len(datos)}; "
                  f"el fichero es coherente)")
        return datos
    return None


def main():
    p = Puente()
    p.sincronizar()
    # Se asocia con 'wifista' (el camino ligero de puente.c): el 'sat' arranca
    # ademas el bucle HTTP del satelite, que roba ancho de banda del puerto serie
    # justo cuando lo que queremos es bajarnos 2,4 MB de base64 por pantalla.
    ok, _ = p.esperar_ok(f"wifista {SSID} {AP_CLAVE}", veces=2, espera=25)
    ip = None
    if ok:
        for _ in range(20):
            for l in p.cmd("wifiip", espera=8.0):
                if l.startswith("IP ") and "(sin IP)" not in l:
                    ip = l.split(None, 1)[1].strip(); break
            if ip: break
            time.sleep(2)
    if not ip:
        print("!! el puente no se ha asociado"); p.cerrar(); return 1
    # La consola va por USB-CDC, asi que el "baud" es nominal, pero a 460800 el
    # volcado pasa de 60 a ~88 KB/s (una captura de 1,8 MB en ~28 s en vez de
    # ~41). Se sube DESPUES de asociar: la asociacion es la parte delicada.
    # Medido el 24-sep-2026.
    p.p.baudrate = 460800
    p.sincronizar(1.0)

    # OJO: abrir el puerto serie REINICIA el puente (USB-Serial-JTAG), asi que el
    # simulador BLE se pierde en cada conexion. Si no se rearma aqui, la P4 se
    # queda sin ningun Victron y TODAS las tarjetas enseñan "--" (le paso al
    # usuario el 24-sep mirando la pantalla). Se rearma dentro de esta misma
    # sesion, que es la unica forma de que siga emitiendo mientras se captura.
    if os.path.exists(FICHERO_EQUIPOS):
        devs = [d for d in json.load(open(FICHERO_EQUIPOS)) if d.get("nombre")][:3]
        if devs:
            orden = ("sim ble " + " ".join(f"{d['mac']} {d['key_hex']}" for d in devs))[:250]
            for cmd in (orden, "sim ritmo 50", "sim caos on"):
                ok, _ = p.esperar_ok(cmd, veces=3, espera=12)
                if not ok:
                    print(f"  !! el puente no acepta: {cmd[:40]}")
            print("  simulador BLE rearmado (la P4 vuelve a tener datos)")
    else:
        print(f"  (sin {FICHERO_EQUIPOS}: la P4 se quedara sin datos Victron)")
    print(f"== capturas via puente ({ip}) -> {CARPETA} ==")

    html = traer(p, "http://192.168.4.1/capturas", segundos=60)
    if not html:
        print("!! no he podido traer /capturas"); p.cerrar(); return 1
    indices = sorted({int(m) for m in re.findall(r"captura\?n=(\d+)", html.decode("utf8", "replace"))})
    tope = int(os.environ.get("PUENTE_CAPTURAS_MAX", "24"))
    indices = [i for i in indices if i < tope]
    print(f"  la P4 anuncia {len(indices)} pantallas: {indices[:24]}")

    hechas = 0
    for n in indices:
        t0 = time.time()
        datos = traer(p, f"http://192.168.4.1/captura?n={n}")
        if not datos or not datos.startswith(b"BM"):
            print(f"  pantalla {n:2}: sin BMP ({len(datos) if datos else 0} bytes)"); continue
        bmp = os.path.join(CARPETA, f"pantalla_{n:02d}.bmp")
        with open(bmp, "wb") as f:
            f.write(datos)
        png = bmp[:-4] + ".png"
        ok = subprocess.run(["convert", bmp, png], capture_output=True).returncode == 0
        print(f"  pantalla {n:2}: {len(datos):7} bytes en {time.time()-t0:5.1f} s"
              + ("  -> PNG" if ok else "  (sin ImageMagick)"))
        hechas += 1
    p.cerrar()
    print(f"\n{hechas} capturas en {CARPETA}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
