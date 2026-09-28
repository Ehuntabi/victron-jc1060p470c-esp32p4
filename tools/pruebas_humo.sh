#!/usr/bin/env bash
#
# pruebas_humo.sh — repaso rapido de los endpoints que usa la app del movil,
# contra la P4 de verdad. Nacio el 27-sep-2026: el borrado de viajes desde la app
# llevaba roto desde la v3.29 (la guarda de /api/viaje exigia "id" numerico a
# todas las operaciones, y "borrar" lleva "carpeta") y no se noto hasta que el
# usuario lo probo con el movil. Esto lo caza en cinco segundos.
#
# NO BORRA NADA: el borrado se prueba con una carpeta que no existe, asi que la
# respuesta buena es 404.
#
# Uso:
#   ./pruebas_humo.sh                 (192.168.4.1, usuario victron; pide la clave)
#   ./pruebas_humo.sh 192.168.4.1 victron miclave
#   P4_PASS=miclave ./pruebas_humo.sh
#
# Se ejecuta desde un PC o un movil conectados al Wi-Fi de la pantalla.
set -uo pipefail

IP="${1:-192.168.4.1}"
USUARIO="${2:-victron}"
CLAVE="${3:-${P4_PASS:-}}"
if [ -z "$CLAVE" ]; then
    read -r -s -p "Clave del portal de la P4 (usuario $USUARIO): " CLAVE; echo
fi

BASE="http://$IP"
PESADO="http://$IP:8081"
AUTH=(-u "$USUARIO:$CLAVE" --max-time 20 -s -o /tmp/humo_cuerpo -w '%{http_code}')

: > /tmp/humo_cuerpo   # para que las trazas no se quejen si curl no llega a escribir
fallos=0
# pide <metodo> <url> [datos]  -> deja el cuerpo en /tmp/humo_cuerpo y saca el codigo
pide() {
    local metodo="$1" url="$2" datos="${3:-}"
    if [ "$metodo" = POST ]; then
        curl "${AUTH[@]}" -X POST -H 'Content-Type: application/json' --data "$datos" "$url"
    else
        curl "${AUTH[@]}" "$url"
    fi
}

comprueba() {   # comprueba <descripcion> <codigo> <esperado...>
    local desc="$1" cod="$2"; shift 2
    for e in "$@"; do
        if [ "$cod" = "$e" ]; then
            printf '  [ok]   %-46s %s\n' "$desc" "$cod"
            return 0
        fi
    done
    printf '  [MAL]  %-46s %s (esperaba %s)\n' "$desc" "$cod" "$*"
    printf '         cuerpo: %.120s\n' "$(cat /tmp/humo_cuerpo 2>/dev/null)"
    fallos=$((fallos + 1))
    return 1
}

echo "== Pruebas de humo contra $BASE =="

# 0) Autenticacion
cod=$(pide GET "$BASE/api/state")
comprueba "GET /api/state" "$cod" 200
case "$(cat /tmp/humo_cuerpo)" in
    *'"battery"'*) echo "         (estado con datos)" ;;
    *) echo "         aviso: el JSON no trae 'battery'";;
esac

# 1) Estado de la vigilancia: los campos que lee la app
cod=$(pide GET "$BASE/ausente")
if comprueba "GET /ausente (estado)" "$cod" 200; then
    for campo in vigilancia motivo salud aviso fotos rotando; do
        if grep -q "\"$campo\"" /tmp/humo_cuerpo; then
            printf '  [ok]   campo %-39s\n' "$campo"
        else
            printf '  [MAL]  falta el campo %s en /ausente\n' "$campo"
            fallos=$((fallos + 1))
        fi
    done
fi

# 2) Los parametros estrictos
cod=$(pide GET "$BASE/ausente?on=0")
comprueba "GET /ausente?on=0 (no debe armar)" "$cod" 400
cod=$(pide GET "$BASE/ausente?loquesea")
comprueba "GET /ausente?loquesea (400)" "$cod" 400

# 3) Galeria en JSON (servidor pesado)
cod=$(pide GET "$PESADO/vigilancia.json")
if comprueba "GET /vigilancia.json" "$cod" 200; then
    for campo in total hay_mas capturas; do
        grep -q "\"$campo\"" /tmp/humo_cuerpo \
            && printf '  [ok]   campo %-39s\n' "$campo" \
            || { printf '  [MAL]  falta el campo %s\n' "$campo"; fallos=$((fallos + 1)); }
    done
fi

# 4) Snapshot en vivo: 200 si hay frame, 503 con motivo si la camara aun no dio
cod=$(pide GET "$BASE/snapshot")
comprueba "GET /snapshot" "$cod" 200 503

# 4b) COLOR DE LA CAMARA: el snapshot se mide y se compara con los valores
#     calibrados. Nacio el 28-sep-2026, cuando el usuario vio la imagen verdosa:
#     R/G y B/G bajos (0,6/0,5) son la firma de "el ISP sin aplicar el balance de
#     blancos" (la IPA no corriendo), no un ajuste fino perdido. Necesita Pillow;
#     si no esta, se salta sin fallar.
cod=$(pide GET "$BASE/snapshot")
if [ "$cod" = 200 ]; then
    cp /tmp/humo_cuerpo /tmp/humo_snapshot.jpg
    if python3 -c "import PIL" 2>/dev/null; then
        python3 - <<'PYCOLOR'
from PIL import Image
import numpy as np
src = "/tmp/humo_snapshot.jpg"
try:
    im = Image.open(src).convert("RGB"); a = np.asarray(im).astype(np.float32)
    r, g, b = a[:, :, 0].mean(), a[:, :, 1].mean(), a[:, :, 2].mean()
    rg, bg = r / max(g, 1e-6), b / max(g, 1e-6)
    print(f"  [dato] color del snapshot: R/G={rg:.2f}  B/G={bg:.2f}  (R={r:.0f} G={g:.0f} B={b:.0f})")
    print("         referencia: neutro 1,02/1,02 | con luz 0,98/0,85 | penumbra 1,64/0,81")
    if rg < 0.80 and bg < 0.70:
        print("  [MAL]  IMAGEN VERDOSA: el ISP no esta aplicando el balance de blancos.")
        print("         Mira en el log del sistema: 'failed to get configuration to")
        print("         initialize ISP controller' o 'esp_video_init OK'. Si no sale")
        print("         ninguno de los dos, revisa el flex CSI (hardware).")
    else:
        print("  [ok]   color dentro de lo normal")
except Exception as e:
    print("  [dato] no pude medir el JPEG:", e)
PYCOLOR
    else
        echo "  [dato] sin Pillow: no mido el color (pip install pillow si lo quieres)"
    fi
else
    comprueba "GET /snapshot para medir color" "$cod" 200
fi

# 5) Borrado de viajes: EL caso que estaba roto.
#    Carpeta que no existe -> 404. Si sale 400 "faltan op o id", el firmware
#    volvio a exigir 'id' a la operacion 'borrar': la app no podra borrar.
cod=$(pide POST "$BASE/api/viaje" '{"op":"borrar","carpeta":"__prueba_humo_no_existe__"}')
comprueba "POST /api/viaje borrar (carpeta inexistente)" "$cod" 404
if [ "$cod" = 400 ] && grep -q "faltan op o id" /tmp/humo_cuerpo; then
    echo "         ESTE ES EL FALLO DEL 27-sep-2026: la guarda de /api/viaje"
    echo "         exige 'id' numerico tambien a 'borrar'. Actualiza el firmware."
fi

# 6) Borrado sin carpeta: 400 y diciendo cual falta
cod=$(pide POST "$BASE/api/viaje" '{"op":"borrar"}')
comprueba "POST /api/viaje borrar sin carpeta" "$cod" 400

# 7) Control: se prueba con un dev que NO existe (400). A proposito NO se usa
#    dev=luz_int: eso ENCIENDE/APAGA la luz interior de verdad, y una prueba de
#    humo no debe tocar nada de la furgo (lo vi releyendo el script: ponia
#    "no borra nada" pero accionaba un interruptor).
cod=$(pide POST "$BASE/control" 'dev=__no_existe__')
comprueba "POST /control dev desconocido (400, no acciona nada)" "$cod" 400

echo
if [ "$fallos" -eq 0 ]; then
    echo "TODO BIEN: los endpoints que usa la app responden lo que espera."
else
    echo "FALLOS: $fallos. Revisa arriba (y el firmware grabado)."
fi
exit $((fallos > 0))
