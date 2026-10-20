#!/usr/bin/env bash
# Corre los tests que no necesitan hardware.
set -euo pipefail
cd "$(dirname "$0")/.."
echo "=== test del protocolo mini_proto ==="
gcc -std=gnu11 -Wall -Wextra -I. -o /tmp/test_mini_proto test/test_mini_proto.c
/tmp/test_mini_proto
echo "=== test de la propiedad del viaje (dueno de las operaciones) ==="
bash test/test_viaje_dueno.sh
echo "=== test del TOKEN de las pantallas (forzar: dev + token) ==="
bash test/test_viaje_token.sh
echo "=== test del cierre del viaje (idempotente: el rename de FatFs no pisa) ==="
bash test/test_viaje_cierre.sh
echo "=== test de ENTRADA HOSTIL (lo que se teclea no es lo que se espera) ==="
# Necesita el cJSON del IDF para la parte de la P4 (igual que test_viaje_dueno):
# en CI el workflow ya pasa IDF_PATH=/tmp/idf; en local lo busca solo en
# ~/.espressif/esp-idf-5.5.
bash test/entrada_hostil.sh
echo "=== test de la validacion del firmware de las pantallas (OTA) ==="
bash test/test_pantalla_fw.sh
