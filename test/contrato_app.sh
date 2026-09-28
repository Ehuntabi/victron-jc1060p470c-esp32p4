#!/usr/bin/env bash
#
# contrato_app.sh — Comprueba, SIN placa y SIN ESP-IDF, que la P4 sigue hablando
# con la app del movil como la app espera. Lo ejecuta el CI en cada push (job
# "contrato_app").
#
# Por que existe: el borrado de viajes estuvo roto desde la v3.29 (la guarda de
# /api/viaje exigia "id" a todas las operaciones y "borrar" lleva "carpeta") y no
# se supo hasta que el usuario lo probo con el movil. El CI ya vigilaba el
# contrato P4<->cabina (mini_proto_sync) pero no este.
#
# Dos partes:
#   1. Compila main/portal/contrato_app.c + test/test_contrato_app.c con el gcc de
#      la maquina y ejecuta las pruebas: es codigo de PRODUCCION (los handlers
#      llaman a esas funciones), no una copia.
#   2. Comprobaciones de texto sobre el firmware: endpoints y puertos que usa la
#      app, valores de /control y campos de /vigilancia.json.
#
# Si algo falla aqui, mira el mensaje y actualiza el lado que ha cambiado (y esta
# tabla, si el cambio es legitimo).
set -uo pipefail

AQUI="$(cd "$(dirname "$0")" && pwd)"
RAIZ="$(cd "$AQUI/.." && pwd)"
CC="${CC:-gcc}"
fallos=0

echo "== 1) Pruebas del modulo del contrato (codigo de produccion) =="
if ! "$CC" -std=gnu17 -Wall -Wextra -Werror -O1 \
        -I "$RAIZ/main/portal" \
        "$RAIZ/main/portal/contrato_app.c" "$AQUI/test_contrato_app.c" \
        -o /tmp/test_contrato_app; then
    echo "  [MAL]  no compila el modulo del contrato"
    exit 1
fi
if /tmp/test_contrato_app; then
    echo "  [ok]   las pruebas pasan"
else
    echo "  [MAL]  hay pruebas que fallan"
    fallos=$((fallos + 1))
fi

echo
echo "== 2) La app usa estos endpoints: existen y en el puerto que toca =="
CS="$RAIZ/main/portal/config_server.c"
comprueba_registro() {   # comprueba_registro <ruta> <servidor>
    local ruta="$1" servidor="$2" vars v
    # El httpd_uri_t puede declararse de las dos formas que hay en el fichero:
    #   httpd_uri_t uri_x = {"/ruta", ...};
    #   httpd_uri_t uri_x = { .uri = "/ruta", ...};
    vars=$(grep -oE "httpd_uri_t[[:space:]]+[a-zA-Z_0-9]+[[:space:]]*=[[:space:]]*\{[^}]*\"$ruta\"" "$CS" \
           | awk '{print $2}')
    if [ -z "$vars" ]; then
        printf '  [MAL]  %-22s no existe en el firmware\n' "$ruta"; fallos=$((fallos + 1)); return
    fi
    # Una ruta puede tener varias variantes (GET y POST); vale que CUALQUIERA
    # este registrada en el servidor que usa la app.
    for v in $vars; do
        if grep -qE "httpd_register_uri_handler\([[:space:]]*$servidor[[:space:]]*,[[:space:]]*&$v[[:space:]]*\)" "$CS"; then
            printf '  [ok]   %-22s en %s (%s)\n' "$ruta" "$servidor" "$v"
            return
        fi
    done
    printf '  [MAL]  %-22s existe pero NO esta registrado en %s\n' "$ruta" "$servidor"
    fallos=$((fallos + 1))
}
comprueba_registro "/api/state"    server
comprueba_registro "/snapshot"     server
comprueba_registro "/ausente"      server
comprueba_registro "/control"      server
comprueba_registro "/api/viaje"    server
comprueba_registro "/data/viajes"  server
comprueba_registro "/vigilancia.json" server_heavy
comprueba_registro "/ota"          server_heavy

echo
echo "== 3) /control: los dev que manda la app =="
for dev in luz_int luz_ext bomba fan; do
    if grep -q "dev=$dev" "$CS"; then
        printf '  [ok]   dev=%s\n' "$dev"
    else
        printf '  [MAL]  el firmware ya no acepta dev=%s\n' "$dev"; fallos=$((fallos + 1))
    fi
done

echo
echo "== 4) /vigilancia.json: los campos que lee la app =="
CV="$RAIZ/main/portal/config_server_vigilancia.c"
for campo in total hay_mas capturas id url kb fecha; do
    if grep -q "\\\\\"$campo\\\\\":" "$CV"; then
        printf '  [ok]   %s\n' "$campo"
    else
        printf '  [MAL]  falta el campo %s en /vigilancia.json\n' "$campo"; fallos=$((fallos + 1))
    fi
done

echo
echo "== 5) El firmware usa el modulo probado (no una copia) =="
if grep -q "ausente_json(" "$CS"; then
    echo "  [ok]   /ausente construye el JSON con ausente_json()"
else
    echo "  [MAL]  /ausente no usa ausente_json(): lo probado no es lo que corre"
    fallos=$((fallos + 1))
fi
if grep -q "viaje_campos_que_faltan(" "$RAIZ/main/portal/config_server_viaje.c"; then
    echo "  [ok]   /api/viaje valida con viaje_campos_que_faltan()"
else
    echo "  [MAL]  /api/viaje no usa viaje_campos_que_faltan(): lo probado no es lo que corre"
    fallos=$((fallos + 1))
fi

echo
if [ "$fallos" -eq 0 ]; then
    echo "CONTRATO OK: la P4 habla con la app como la app espera."
else
    echo "CONTRATO ROTO: $fallos comprobaciones han fallado (ver arriba)."
fi
exit $((fallos > 0))
