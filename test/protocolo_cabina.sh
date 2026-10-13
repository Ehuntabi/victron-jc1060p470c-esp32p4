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
# SE SALTA p4_test.c A PROPOSITO (13-oct-2026): ese fichero es el AUTO-TEST de
# diagnostico y sus cuerpos son de mentira a proposito -- uno manda una op que
# la P4 no conoce PARA COMPROBAR que contesta 4xx, y otro va con credenciales
# malas para comprobar el 401. No son parte del contrato entre las dos placas:
# meterlos aqui daba cinco fallos que no eran fallos, y tapaban los de verdad.
ops=$(grep -rho --exclude=p4_test.c '\\"op\\": *\\"[a-z_]*\\"' "$CAB/main" 2>/dev/null |
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
campos=$(grep -rho --exclude=p4_test.c '\\"[a-z_]\{2,\}\\"' "$CAB/main/net" "$CAB/main" 2>/dev/null |
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

# ── 3b. LOS CAMPOS DE DINERO, UNO A UNO (auditoria del 13-oct-2026) ──────────
# La regla de arriba es ASPERSA: da por bueno un campo con que su nombre aparezca
# en cualquier sitio de la P4 (aunque sea en un comentario o en otro contexto), y
# no distingue de que operacion es. Con el dinero eso no vale: si alguien renombra
# "importe" y el nombre nuevo aparece por casualidad en la P4, el resumen del
# viaje sale con 0 EUR y el dato SI esta en los CSV -- se descubre en casa,
# semanas despues. Aqui se exige que cada campo que mueve dinero lo LEA la P4
# como campo de 'datos' (cJSON_GetObjectItem... "X") o con IMPORTE_DE("X").
for c in precio coste importe litros moneda noches; do
    if grep -rq -e "datos, \"$c\"" -e "IMPORTE_DE(\"$c\")" "$VICT/main/portal/"; then
        ok "campo de dinero '$c': la P4 lo lee de 'datos'"
    else
        mal "campo de dinero '$c': la P4 NO lo lee como campo de 'datos' (¿renombrado?)"
    fi
done

# ── 3c. El canal de alarmas (aqui se colo el fallo del 'silencio') ───────────
# La cabina manda {"alarmas":N,"silencio":0|1}: los dos tienen que estar en el
# handler de la P4. El TIPO no se puede comprobar desde aqui (fue justo el
# fallo: la cabina mandaba numero y la P4 esperaba booleano), pero que el campo
# exista si.
for c in alarmas silencio; do
    if grep -rq "\"$c\"" "$VICT/main/portal/config_server.c"; then
        ok "campo de alarmas '$c' presente en el handler de la P4"
    else
        mal "la cabina manda '$c' a /api/alarma y la P4 no lo lee"
    fi
done

echo
if [ "$fallos" -eq 0 ]; then
    echo "TODO OK: la cabina y la P4 hablan el mismo idioma."
    exit 0
fi
echo "FALLOS: $fallos. Revisa el contrato entre las dos placas antes de publicar."
exit 1
