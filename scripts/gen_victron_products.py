#!/usr/bin/env python3
"""Regenera la tabla de nombres de producto Victron del componente victron_ble.

La fuente es `Victron_ProductId_mapping.txt` del repositorio keshavdv/victron-ble,
que es la lista de ids de producto extraida del binario oficial vecan-dbus (la
misma que usa Victron en sus aparatos). Antes esta tabla se genero a mano desde
la lista corta del componente de ESPHome, que no traia modelos como el Orion, los
Blue Smart, los MultiPlus ni los Lynx.

Escribe:
    components/victron_ble/include/victron_products.h   (enum de ids)
    components/victron_ble/victron_products.c           (tabla id -> nombre)

Uso:
    python3 scripts/gen_victron_products.py                 # descarga la lista
    python3 scripts/gen_victron_products.py --fichero x.txt # usa una copia local
    python3 scripts/gen_victron_products.py --comprobar     # no escribe, solo informa
"""
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
import urllib.request
from pathlib import Path

FUENTE = ("https://raw.githubusercontent.com/keshavdv/victron-ble/main/"
          "Victron_ProductId_mapping.txt")
RAIZ = Path(__file__).resolve().parent.parent
COMP = RAIZ / "components" / "victron_ble"
H = COMP / "include" / "victron_products.h"
C = COMP / "victron_products.c"

# Entradas de la lista oficial que no son aparatos: software, interfaces y reservados.
NO_ES_APARATO = re.compile(
    r"^(sw\b|vpn$|vbc|vvc|vcc$|vcm$|vgm$|vrs$|reserved|no product set|"
    r"free technics|unknown$|test\b|demo\b|dummy)", re.I)


def descargar(destino: Path) -> Path:
    print(f"descargando {FUENTE}")
    with urllib.request.urlopen(FUENTE, timeout=30) as r:
        destino.write_bytes(r.read())
    return destino


def leer(ruta: Path) -> tuple[dict[int, str], int]:
    """Devuelve {id: nombre} y cuántas líneas se han descartado."""
    productos: dict[int, str] = {}
    descartados = 0
    for linea in ruta.read_text(encoding="utf-8", errors="replace").splitlines():
        linea = linea.strip()
        if not linea or linea.startswith("#"):
            continue
        partes = linea.split(None, 1)
        if len(partes) != 2 or not partes[0].isdigit():
            continue
        pid, nombre = int(partes[0]), partes[1].strip()
        if pid == 0 or pid >= 0xF000 or not nombre:
            descartados += 1
            continue
        if NO_ES_APARATO.match(nombre):
            descartados += 1
            continue
        nombre = re.sub(r"\s+", " ", nombre.replace("|", "/")).strip()
        if pid in productos:
            descartados += 1
            continue
        productos[pid] = nombre
    return productos, descartados


def identificador(nombre: str) -> str:
    ident = re.sub(r"[^0-9A-Za-z]+", "_", nombre).strip("_")
    if not ident or ident[0].isdigit():
        ident = "P_" + ident
    return ident


def leer_actual() -> dict[int, str]:
    if not C.exists():
        return {}
    actual = {}
    for m in re.finditer(r'\{\s*(0x[0-9A-Fa-f]+),\s*"([^"]+)"\s*\}', C.read_text()):
        actual[int(m.group(1), 16)] = m.group(2)
    return actual


def escribir_h(productos: dict[int, str]) -> None:
    hoy = dt.date.today().isoformat()
    lineas = [
        "#pragma once",
        "// Generado por scripts/gen_victron_products.py — no editar a mano.",
        f"// Fuente: lista oficial de ids de producto de Victron (vecan-dbus), {hoy}.",
        "// " + str(len(productos)) + " productos.",
        "#include <stdint.h>",
        "typedef enum {",
        "    VICTRON_PRODUCT_UNKNOWN = 0x0000,",
    ]
    vistos: set[str] = set()
    for pid, nombre in sorted(productos.items()):
        ident = identificador(nombre)
        if ident in vistos:          # nombres repetidos para ids distintos
            ident = f"{ident}_{pid:04X}"
        vistos.add(ident)
        lineas.append(f"    VICTRON_PRODUCT_{ident} = 0x{pid:04X}, // {nombre}")
    lineas += [
        "} victron_product_id_t;",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "const char *victron_product_name(uint16_t product_id);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    H.write_text("\n".join(lineas) + "\n", encoding="utf-8")


def escribir_c(productos: dict[int, str]) -> None:
    hoy = dt.date.today().isoformat()
    lineas = [
        "// Generado por scripts/gen_victron_products.py — no editar a mano.",
        f"// Fuente: lista oficial de ids de producto de Victron (vecan-dbus), {hoy}.",
        '#include "victron_products.h"',
        "#include <stddef.h>",
        "",
        "typedef struct {",
        "    uint16_t id;",
        "    const char *name;",
        "} victron_product_name_entry_t;",
        "",
        f"static const victron_product_name_entry_t kProductNames[{len(productos)}] = {{",
    ]
    for pid, nombre in sorted(productos.items()):
        lineas.append(f'    {{ 0x{pid:04X}, "{nombre}" }},')
    lineas += [
        "};",
        "",
        "const char *victron_product_name(uint16_t product_id)",
        "{",
        "    for (size_t i = 0; i < (sizeof(kProductNames) / sizeof(kProductNames[0])); ++i) {",
        "        if (kProductNames[i].id == product_id) {",
        "            return kProductNames[i].name;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    C.write_text("\n".join(lineas), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fichero", type=Path, default=None,
                    help="copia local de Victron_ProductId_mapping.txt")
    ap.add_argument("--comprobar", action="store_true",
                    help="no escribe nada, solo dice qué cambiaría")
    ap.add_argument("--conservar-nombres", action="store_true",
                    help="mantiene los nombres que ya estaban para los ids repetidos")
    args = ap.parse_args()

    if not COMP.is_dir():
        raise SystemExit(f"no encuentro {COMP}")

    ruta = args.fichero or descargar(Path("/tmp/Victron_ProductId_mapping.txt"))
    productos, descartados = leer(ruta)
    if len(productos) < 100:
        raise SystemExit(f"solo {len(productos)} productos: la lista no parece buena")
    actual = leer_actual()

    # los que ya estaban y no salen en la lista oficial no se pierden
    extra = {k: v for k, v in actual.items() if k not in productos}
    productos.update(extra)
    if args.conservar_nombres:
        for k, v in actual.items():
            if k in productos:
                productos[k] = v
    productos = dict(sorted(productos.items()))

    nuevos = {k: v for k, v in productos.items() if k not in actual}
    fuera = {k: v for k, v in actual.items() if k not in productos}
    renombrados = {k: (actual[k], productos[k]) for k in actual
                   if k in productos and actual[k] != productos[k]}

    print(f"productos en la lista oficial: {len(productos) - len(extra)} "
          f"({descartados} lineas descartadas)")
    if extra:
        print(f"conservados del firmware (no estan en la lista oficial): {len(extra)}")
        for k, v in list(extra.items())[:5]:
            print(f"  = 0x{k:04X} {v}")
    print(f"tabla actual: {len(actual)}  ·  nuevos: {len(nuevos)}  ·  "
          f"renombrados: {len(renombrados)}  ·  desaparecen: {len(fuera)}")
    for k, v in sorted(nuevos.items())[:12]:
        print(f"  + 0x{k:04X} {v}")
    if len(nuevos) > 12:
        print(f"  ... y {len(nuevos) - 12} mas")
    for k, (a, b) in sorted(renombrados.items())[:8]:
        print(f"  ~ 0x{k:04X} {a!r} -> {b!r}")
    for k, v in sorted(fuera.items())[:8]:
        print(f"  - 0x{k:04X} {v}")

    if args.comprobar:
        print("(--comprobar: no se ha escrito nada)")
        return 0

    escribir_h(productos)
    escribir_c(productos)
    print(f"escritos {H.relative_to(RAIZ)} y {C.relative_to(RAIZ)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
