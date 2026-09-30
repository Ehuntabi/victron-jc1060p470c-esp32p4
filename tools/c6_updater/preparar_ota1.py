#!/usr/bin/env python3
"""Prepara la imagen de la radio para la particion ota_1.

Delante van 16 bytes: magic 'RADIO1', el tamano de la imagen y su crc32. El
modulo slave_ota.c los comprueba antes de grabar nada en el C6, asi que si en la
particion no esta lo que tiene que estar, no toca la radio.

Uso:
    python3 preparar_ota1.py ~/joint/firmware_radio/network_adapter_2.12.13.bin salida.bin
"""
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"RADIO1\0\0"
TAM_MIN = 900 * 1024
TAM_MAX = 2 * 1024 * 1024


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    origen, destino = Path(sys.argv[1]), Path(sys.argv[2])
    if not origen.is_file():
        print(f"ERROR: no encuentro {origen}")
        return 1
    img = origen.read_bytes()
    if not (TAM_MIN <= len(img) <= TAM_MAX):
        print(f"ERROR: {len(img)} bytes no parece la imagen de la radio "
              f"(se esperan entre {TAM_MIN} y {TAM_MAX})")
        return 1
    crc = zlib.crc32(img) & 0xFFFFFFFF
    destino.write_bytes(MAGIC + struct.pack("<II", len(img), crc) + img)
    print(f"   {origen.name}: {len(img)} bytes, crc 0x{crc:08x} "
          f"-> {destino} ({destino.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
