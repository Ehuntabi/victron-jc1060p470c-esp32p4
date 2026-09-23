#!/usr/bin/env bash
#
# aplicar.sh — Prepara (y opcionalmente graba) el firmware que actualiza el C6
# de una placa que todavia lleva la radio vieja.
#
# POR QUE HACE FALTA
# El firmware nuevo de la P4 lleva esp_hosted 2.12.13, y su host y el esclavo
# (C6) van EMPARREADOS: con un C6 viejo el AP funciona pero el BLE no arranca
# (medido el 23-sep-2026). Ademas, el firmware nuevo YA NO SABE grabar el C6
# viejo: la API de OTA cambio en la 2.6.0 (el "end" ya no activa). Por eso hay
# que grabar el C6 ANTES, con el firmware VIEJO en la placa.
#
# QUE HACE
#   1. Saca un arbol limpio del firmware viejo (tag v3.14) en .scratch/c6_carrier
#      (git worktree: no toca tu copia de trabajo).
#   2. Le inyecta este modulo (slave_ota.c/h), lo llama al arrancar y anade el
#      boton en Ajustes -> Wi-Fi.
#   3. Compila.
#   4. Si le dices "flash", lo graba por USB.
#
# DESPUES (en la placa):
#   - Copia network_adapter.bin a la RAIZ de la tarjeta SD (el fichero esta en
#     ~/c6_backup_2026-09-23/ junto a la imagen vieja, por si hay que volver).
#   - Arranca la placa: a los 30 s graba el C6 sola (o pulsa Ajustes -> Wi-Fi ->
#     "Actualizar radio C6"). Tarda ~1 minuto y reinicia los dos.
#   - Comprueba en el arranque siguiente que aparece EVENT: 16 (eso es que el C6
#     lleva ya el firmware nuevo).
#   - Por ultimo graba el firmware nuevo de la P4: al arrancar tiene que decir
#     "Radio C6: firmware 2.12.13".
#
# Uso:
#   ./aplicar.sh build     # solo compila
#   ./aplicar.sh flash     # compila y graba
#
set -euo pipefail

REPO="${REPO:-$HOME/joint/victron}"
TAG="${TAG:-v3.14}"                 # ultimo firmware con el host viejo (0.0.27)
ARBOL="${ARBOL:-$HOME/joint/.scratch/c6_carrier}"
PUERTO="${PUERTO:-}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/esp-idf-5.5/export.sh}"

QUE="${1:-build}"
case "$QUE" in
    build|flash) ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "ERROR: usa 'build' o 'flash'"; exit 1 ;;
esac

paso() { printf '\n\033[1m── %s\033[0m\n' "$*"; }
ok()   { printf '   [ok] %s\n' "$*"; }
mal()  { printf '   [!!] %s\n' "$*" >&2; }

[ -f "$IDF_EXPORT" ] || { mal "no encuentro $IDF_EXPORT"; exit 1; }

paso "1. Arbol del firmware viejo ($TAG)"
if [ -d "$ARBOL/.git" ] || [ -f "$ARBOL/.git" ]; then
    ok "ya existe $ARBOL"
else
    mkdir -p "$(dirname "$ARBOL")"
    git -C "$REPO" worktree add --detach "$ARBOL" "$TAG"
    ok "creado $ARBOL desde $TAG"
fi
grep -m1 '^version' "$ARBOL/components/espressif__esp_hosted/idf_component.yml" \
    | sed 's/^/   esp_hosted del arbol: /'

paso "2. Inyectar el actualizador"
mkdir -p "$ARBOL/main/portal"
cp -f "$(dirname "$0")/slave_ota.c" "$ARBOL/main/portal/slave_ota.c"
cp -f "$(dirname "$0")/slave_ota.h" "$ARBOL/main/portal/slave_ota.h"
python3 - "$ARBOL" <<'PY'
import sys
from pathlib import Path
arbol = Path(sys.argv[1])

# --- main.c: include + arranque diferido ---
p = arbol / "main/main.c"
t = p.read_text(encoding="utf8")
if "slave_ota.h" not in t:
    a = '#include "portal/config_server.h"\n'
    assert t.count(a) == 1, "no encuentro el include de config_server en main.c"
    t = t.replace(a, a + '#include "portal/slave_ota.h"\n', 1)
    ancla = 'logSection("Setup complete");\n    mark_boot_successful();\n}'
    assert t.count(ancla) == 1, "no encuentro el final de app_main"
    t = t.replace(ancla,
        '    /* HERRAMIENTA DE MANTENIMIENTO (tools/c6_updater): si hay imagen del\n'
        '     * C6 en la SD, la graba sola a los 30 s. Ver LEEME.md. */\n'
        '    slave_ota_start_diferido(30);\n\n' + ancla, 1)
    p.write_text(t, encoding="utf8")
    print("   [ok] main.c")
else:
    print("   [ok] main.c (ya estaba)")

# --- settings_wifi.c: boton ---
p = arbol / "main/ui/settings/settings_wifi.c"
t = p.read_text(encoding="utf8")
if "slave_ota_btn_cb" not in t:
    a = '#include "settings_common.h"\n'
    assert t.count(a) >= 1, "no encuentro settings_common.h en settings_wifi.c"
    i = t.index(a) + len(a)
    t = t[:i] + '#include "../../portal/slave_ota.h"\n' + t[i:]
    d = "static void slave_ota_btn_cb(lv_event_t *e);\n"
    a2 = "static void wifi_pintar_ip_ap(ui_state_t *ui);\n"
    i = t.index(a2) + len(a2)
    t = t[:i] + d + t[i:]
    a3 = "    lv_obj_update_layout(cont);\n    lv_coord_t h_ap = lv_obj_get_height(card1);"
    assert t.count(a3) == 1, "no encuentro el final de la pagina Wi-Fi"
    boton = '''    /* HERRAMIENTA DE MANTENIMIENTO: graba el firmware del C6 desde la SD.
     * En rojo a proposito: es lo unico de esta pantalla que puede dejar la
     * placa sin Wi-Fi y sin BLE si sale mal. Ver tools/c6_updater/LEEME.md. */
    lv_obj_t *btn_c6 = lv_btn_create(card1);
    lv_obj_set_width(btn_c6, lv_pct(100));
    lv_obj_set_style_bg_color(btn_c6, lv_color_hex(0x8B2E2E), 0);
    lv_obj_add_event_cb(btn_c6, slave_ota_btn_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_c6 = lv_label_create(btn_c6);
    lv_obj_set_style_text_font(lbl_c6, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_c6, "Actualizar radio C6");
    lv_obj_center(lbl_c6);

'''
    t = t.replace(a3, boton + a3, 1)
    t += '''
/* HERRAMIENTA DE MANTENIMIENTO: ver tools/c6_updater/LEEME.md. */
static void slave_ota_btn_cb(lv_event_t *e)
{
    (void)e;
    if (slave_ota_en_curso()) {
        ESP_LOGW(TAG_SETTINGS, "ya hay una grabacion del C6 en marcha");
        return;
    }
    ESP_LOGW(TAG_SETTINGS, "grabacion del C6 pedida desde Ajustes");
    slave_ota_start();
}
'''
    p.write_text(t, encoding="utf8")
    print("   [ok] settings_wifi.c (boton)")
else:
    print("   [ok] settings_wifi.c (ya estaba)")
PY

paso "3. Compilar"
# shellcheck disable=SC1090
source "$IDF_EXPORT" >/dev/null 2>&1
(cd "$ARBOL" && idf.py build 2>&1 | tail -5)

if [ "$QUE" = "flash" ]; then
    paso "4. Grabar por USB"
    if [ -z "$PUERTO" ]; then
        PUERTO=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)
    fi
    [ -n "$PUERTO" ] || { mal "no hay puerto serie: enchufa la placa"; exit 1; }
    ok "puerto $PUERTO"
    (cd "$ARBOL" && idf.py -p "$PUERTO" flash 2>&1 | tail -4)
    cat <<'FIN'

── AHORA, EN LA PLACA ────────────────────────────────────────────────────────
  1. Copia network_adapter.bin a la RAIZ de la tarjeta SD y vuelve a meterla.
     (La imagen buena esta en ~/esp_hosted_21213/slave/build_c6/network_adapter.bin)
  2. Reinicia la placa. A los 30 s graba el C6 sola; tambien puedes pulsar
     Ajustes -> Wi-Fi -> "Actualizar radio C6". Tarda ~1 minuto.
  3. Cuando termine, graba el firmware nuevo de la P4 (el de publicacion).
  4. En el log del arranque tiene que salir:  Radio C6: firmware 2.12.13
     Si sale "no dice su version", el C6 sigue viejo: repite el paso 1-2.
FIN
fi
