#!/usr/bin/env bash
#
# protocolo_cabina.sh — Comprueba, SIN PLACA, que la 35cabina y la P4 siguen
# hablando el mismo idioma. Es el guardián del contrato entre las dos placas,
# hermano de contrato_app.sh (que mira el de la app).
#
# POR QUE EXISTE
# El 30-sep-2026 se quedo un viaje sin poder enviarse: la P4 de la autocaravana
# llevaba una version vieja y rechazaba los apuntes de la cabina. Nada de eso
# salta al compilar, porque son dos repos distintos: uno manda un JSON y el otro
# lo interpreta. Esto lo comprueba aqui.
#
# QUE COMPRUEBA
#   1. Los dos mini_proto.h son IDENTICOS. El struct del protocolo UDP viaja byte
#      a byte entre las dos placas: si uno cambia y el otro no, la cabina rechaza
#      todo lo que le llega (comprueba version y crc) y se queda muda.
#   2. Cada operacion que manda la cabina ("inicio", "fin", "registro",
#      "descartar") la conoce la P4.
#   3. Cada campo del JSON que manda la cabina aparece en el portal de la P4.
#
# Uso:  bash test/protocolo_cabina.sh [ruta_victron] [ruta_35cabina]
#       (por defecto las busca como hermanas, que es como las deja el CI)
set -uo pipefail

VICT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
CAB="${2:-$(cd "$(dirname "$0")/../../35cabina" 2>/dev/null && pwd || echo "")}"

fallos=0
ok()   { printf '  [ok] %s\n' "$*"; }
mal()  { printf '  [!!] %s\n' "$*"; fallos=$((fallos + 1)); }

[ -d "$VICT" ] || { echo "ERROR: no encuentro el repo victron en $VICT"; exit 2; }
[ -n "$CAB" ] && [ -d "$CAB" ] || { echo "ERROR: no encuentro el repo 35cabina (pasalo como 2o argumento)"; exit 2; }

echo "== 1. El protocolo UDP es el mismo en las dos placas =="
if diff -q "$VICT/main/net/mini_proto.h" "$CAB/main/net/mini_proto.h" >/dev/null 2>&1; then
    ver_v=$(grep -oP '#define\s+MINI_PROTO_VERSION\s+\K[0-9]+' "$VICT/main/net/mini_proto.h")
    ok "mini_proto.h identicos (version $ver_v)"
else
    mal "mini_proto.h se ha separado: la cabina rechazaria los mensajes de la P4"
    diff -u "$VICT/main/net/mini_proto.h" "$CAB/main/net/mini_proto.h" | head -20 | sed 's/^/       /'
fi

echo "== 2. Las operaciones que manda la cabina las conoce la P4 =="
ops=$(grep -rho '\\"op\\": *\\"[a-z_]*\\"' "$CAB/main" 2>/dev/null |
      grep -o '[a-z_]*\\"$' | tr -d '\\"' | sort -u)
[ -n "$ops" ] || mal "no he sabido sacar ninguna operacion de la cabina (¿cambio el formato?)"
for op in $ops; do
    if grep -rq "\"$op\"" "$VICT/main/portal/"; then
        ok "op '$op'"
    else
        mal "la cabina manda la op '$op' y la P4 no la conoce"
    fi
done

echo "== 3. Los campos del JSON que manda la cabina existen en la P4 =="
# Se excluyen los que son del protocolo o del sobre, no del contenido.
excluir="op ts id alarmas"
campos=$(grep -rho '\\"[a-z_]\{2,\}\\"' "$CAB/main/net" "$CAB/main" 2>/dev/null |
         tr -d '\\"' | sort -u)
[ -n "$campos" ] || mal "no he sabido sacar ningun campo de la cabina"
for c in $campos; do
    case " $excluir " in *" $c "*) continue ;; esac
    if grep -rq "\"$c\"" "$VICT/main/portal/" "$VICT/main/net/"; then
        :
    else
        mal "la cabina manda el campo '$c' y la P4 no lo menciona en ningun sitio"
    fi
done
n_campos=$(echo "$campos" | wc -w)
ok "$n_campos campos comprobados"

echo
if [ "$fallos" -eq 0 ]; then
    echo "TODO OK: la cabina y la P4 hablan el mismo idioma."
    exit 0
fi
echo "FALLOS: $fallos. Revisa el contrato entre las dos placas antes de publicar."
exit 1
