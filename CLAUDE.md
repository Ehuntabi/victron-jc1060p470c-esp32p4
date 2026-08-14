# Joint SPL 145 Control (ex-VictronSolarDisplay) - Guition JC1060P470C_I

Repo: github.com/Ehuntabi/victron-jc1060p470c-esp32p4 · Último tag: **v1.4.28**

> Antes de cualquier trabajo de código no trivial aplicar
> [`andrej-karpathy-skills:karpathy-guidelines`](https://github.com/multica-ai/andrej-karpathy-skills):
> Think Before Coding · Simplicity First · Surgical Changes · Goal-Driven Execution.

## Hardware
- ESP32-P4 (principal) + ESP32-C6 vía SDIO (Wi-Fi/BT con esp_hosted)
- Display DSI 1024x600 (panel JD9165BA)
- Touch GT911, RTC RX8025T (I²C 0x32, pila CR1220), microSD slot 0 IOMUX
- Codec audio ES8311 + amplificador NS4150 (GPIO11 PA_CTRL)
- Pines I2S: MCLK=GPIO13, BCLK=GPIO12, LRCK=GPIO10, DOUT=GPIO9
- Ventilador frigo PWM en GPIO5 (JP1 pin 15)
- Bus 1-Wire DS18B20 en GPIO4 (JP1 pin 13, pullup 4.7K a 3.3V)

## Stack
- ESP-IDF v5.4.4
- LVGL para la UI
- Workspace: ~/joint/victron

## Comandos habituales
- Entorno IDF (necesario antes de compilar/flashear): `. ~/.espressif/esp-idf-5.4/export.sh`
- Compilar: `idf.py build`
- Flashear: `idf.py -p /dev/ttyACM0 flash`  (el puerto varia: ttyACM0 o ttyACM1)
- Monitor: `idf.py -p /dev/ttyACM0 monitor`

## Versionado / releases
- La version que se ve en Ajustes -> Acerca de sale SOLA de `git describe`
  (esp_app_get_description()->version). NO se edita ningun numero en el codigo.
- Para publicar una version nueva: `./release.sh X.Y.Z` (crea el tag, build limpio
  anti-gotcha, imagen fusionada, y recuerda push + crear Release + flashear).
- Gotcha ESP-IDF: la version/fecha se cachea en el configure de CMake; si el About
  muestra datos viejos, `idf.py reconfigure` + borrar el .obj de esp_app_desc lo
  refresca (release.sh ya lo hace).

## Convenciones del proyecto
- Estética card-based aplicada en Settings (cada página con su color de borde)
- Textos en español, sin emojis (excepto símbolos LV_SYMBOL_*)
- Persistencia con NVS por componente (namespace propio)
- Logs sin acentos para evitar problemas de codificación

## Estructura clave
- main/main.c: app_main() son 10 llamadas a init_*() en fases (una por
  seccion del arranque: display/UI, SD+RTC+frigo, red, telemetria,
  audio+BLE, timers periodicos, camara...), todas en el mismo fichero
- main/ui.c: tabview, barra inferior, screensaver, hora
- main/ui/ agrupado por tema (2026-08-14, antes plano):
  - settings/: settings_panel.c (orquestador: paginas SD, Autocaravana,
    screensaver/modo nocturno) + paginas concretas (settings_wifi.c,
    settings_display.c, settings_victron_keys.c, settings_about.c,
    settings_sound.c, trip_manager.c, settings_dialogs.c)
  - views/: frigo_panel.c y el resto de pantallas (view_overview.c,
    view_battery_monitor.c, view_simple*.c, view_solar_charger.c...)
  - devices/: device_tracker.c, ble_ingest.c
  - history/: battery_history_screen.c, frigo_history_screen.c, chart_common.c
  - vigilancia/: ausente_mode.c, gallery.c, capture_carousel.c
  - widgets/: ui_card.c, ui_format.c, ui_state.h, lv_font_thermometer.c
- main/config_server.c: portal HTTP — handlers core (root, keys, static,
  save, api_state, dashboard, settime, capturas) + arranque/registro de URIs
  del httpd y del DNS del captive portal
- main/config_server_ap.c: ciclo de vida del AP Wi-Fi — radio (wifi_ap_init),
  timers de auto-off del HTTP, cola de trabajos (start/stop/apply); expone
  cfg_http_stop()/cfg_dns_stop() (definidas en config_server.c) para pararlo
  sin tocar sus estaticos
- lo autocontenido vive aparte: charts_svg.c, data_export_tar.c,
  config_server_vigilancia.c (galeria /snapshot y /vigilancia),
  config_server_auth.c (Basic Auth + servido de SPIFFS)
- components/audio_es8311/: codec con jingles BOOT_OK/CRITICAL/WARNING/CONFIRM
- components/alerts/: thresholds NVS (freezer/SoC)
- components/config_storage/: persistencia general (Wi-Fi, screensaver, etc.)

## Pendientes activos
1. **PWM del ventilador a 25 kHz — NO es solo cambiar la línea** (27-jul-2026).
   Ahora está a 18 kHz (`FRIGO_FAN_FREQ_HZ` en `components/frigo/frigo.h`): inaudible
   para adultos, pero **NO para oídos jóvenes** (su límite ronda los 19-20 kHz). Por
   encima de 20 kHz no lo oye nadie.
   **⚠️ EL CUELLO DE BOTELLA ES EL PC817, NO EL MOSFET.** El ataque actual es
   P4 → **optoacoplador PC817** → **IRLR7843** (R serie en puerta + R pull-down + diodo
   1N4148 de rueda libre). El IRLR7843 conmuta en ns y LEDC llega a ~39 kHz con los
   10 bits actuales (sin perder finura), pero el **PC817 tarda microsegundos**, y el
   apagado bastante más que el encendido. A 18 kHz el ciclo son 55 µs y a 25 kHz solo
   40 µs, así que las transiciones del opto se comen una fracción cada vez mayor.
   **Se degrada sobre todo a duty bajo**: al 30 % el pulso dura ~12 µs y el opto casi no
   llega a conmutar → control no lineal por abajo.
   Por tanto, para subir a 25 kHz hay que **tocar el hardware primero**, eligiendo una:
   (a) sustituir el PC817 por un opto rápido (6N137, o driver aislado tipo TLP250);
   (b) quitar el opto si no hace falta aislamiento — el IRLR7843 es de puerta lógica y
       el P4 puede atacarlo directo con la R serie (la opción más simple);
   (c) quedarse en 18 kHz.
   Al cambiarlo, comprobar en el frigo real: (1) que **sigue arrancando desde parado**
   (hay kickstart 700 ms al 100 % y suelo de duty 30 %) y (2) que el MOSFET no calienta.
   Si arranca peor, volver a 18 kHz.
   (Histórico: antes del IRLR7843 había un módulo D4184, limitado a ≤20 kHz. Ese límite
   ya no aplica, pero apareció el del opto.)
