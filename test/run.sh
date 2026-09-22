#!/usr/bin/env bash
# Corre los tests que no necesitan hardware.
set -euo pipefail
cd "$(dirname "$0")/.."
echo "=== test del protocolo mini_proto ==="
gcc -std=gnu11 -Wall -Wextra -I. -o /tmp/test_mini_proto test/test_mini_proto.c
/tmp/test_mini_proto
