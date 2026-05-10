# VictronSolarDisplay — Port for Guition JC1060P470C_I (ESP32-P4)

**[Español](#español) | [English](#english)**

---

## Español

Port del proyecto [victronsolardisplayesp (multi-device)](https://github.com/CamdenSutherland/victronsolardisplayesp/tree/multi-device) de CamdenSutherland, que a su vez es un fork de [VictronSolarDisplayEsp](https://github.com/wytr/VictronSolarDisplayEsp) de wytr.

Port realizado por **[Ehuntabi](https://github.com/Ehuntabi)**.

---

### Hardware

| Componente | Detalle |
|---|---|
| **Pantalla** | Guition JC1060P470C_I (7", 1024x600, DSI, touch GT911) |
| **SoC principal** | ESP32-P4 |
| **SoC WiFi/BT** | ESP32-C6 (vía SDIO, gestionado por esp_hosted, SDMMC slot 1: CLK=18, CMD=19, D0..D3=14..17) |
| **RTC** | RX8025T — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (compartido con GT911) |
| **Tarjeta microSD** | SDMMC slot 0 IOMUX (CLK=43, CMD=44, D0..D3=39..42), alimentación por LDO interno ch4 (3.3V) |
| **Sensores temperatura** | DS18B20 1-Wire en GPIO26 |
| **Control ventilador** | PWM LEDC en GPIO9, 25KHz |
| **Audio** | Codec ES8311 + amplificador NS4150 (PA_CTRL en GPIO11). I2S MCLK=13/BCLK=12/LRCK=10/DOUT=9 |
| **Batería RTC** | CR1220 |

---

### Funcionalidades

#### UI estilo Venus OS
- Lenguaje visual unificado tipo Venus OS de Victron: cards oscuras con borde de color por rol (cyan/verde/naranja/rojo), métricas con número grande + unidad pequeña, pills de estado, gauges circulares para SOC y sombras suaves.
- **Iconos PNG propios** en los headers de las cards (batería de coche, panel solar, conversor DC/DC, casa para cargas) generados como `lv_img_dsc_t` embebidos en flash.
- Helpers reutilizables (`main/ui/ui_card.h`): `ui_card_create`, `ui_card_set_title_img`, `ui_metric_create_compact/_large`, `ui_pill_create`, `ui_arc_soc_create`, paleta semántica `UI_COLOR_*`.
- Pestañas Live/Settings ocultas — navegación con icono ⚙ discreto en la barra inferior + swipe horizontal del tabview.
- **Auto-vuelta a Live** tras 60 s sin actividad táctil (no actúa si el screensaver está activo).

#### Vistas Live
- **Overview** (vista por defecto): diagrama vertical Solar → Batería → Cargas con flechas y potencia W; arc SOC central + corriente.
- **Battery Monitor**: card naranja con arc SOC grande, V/A/W (font_46) y footer TTG/Consumido/Aux. Tap abre histórico 24h.
- **Solar Charger**: 2 cards (Solar verde + Salida batería naranja) con pill de estado del cargador (Bulk/Absorción/Float).
- **Default Battery**: 3 cards en fila (DC/DC cyan, Batería naranja con arc, Solar verde) con todos los datos consolidados.
- **Simple devices** (Inverter, Smart Lithium, AC Charger, etc.): card único con métricas apiladas; color e icono según el tipo.

#### Settings
Páginas con cards de borde de color y diálogos modales:
- **Frigo** (verde): control PWM del ventilador en función de T_Aletas (DS18B20). T_Exterior y T_Congelador informativos.
- **Logs** (naranja): chart 24 h de batería (con downsample max/min/avg) y de temperaturas frigo.
- **Wi-Fi** (azul): SSID, contraseña, switch on/off; al cambiar muestra modal "Cambio en Wi-Fi requiere reiniciar" con Cancelar / Reiniciar.
- **Display** (morado): brillo pantalla y brillo en reposo en pasos de 5; selector de modo screensaver (Atenuar / Rotar vistas) y periodo.
- **Sonido y avisos** (rojo): volumen, switch silenciar avisos, umbrales de SoC y temperatura del frigorífico.
- **Victron Keys** (rosa): MAC + clave AES por dispositivo (hasta 8); diálogo de aviso al entrar.
- **About** (gris): uptime, RAM libre, IP del AP, chip, versión IDF, botón Reboot con confirmación.

#### Barra inferior
- Posiciones fijas (anchos definidos, `flex_align SPACE_BETWEEN`).
- Reloj + fecha (sincronizado con RTC, refrescado cada 30 s; refresco inmediato tras `settime`).
- Iconos de **estado interactivos**:
  - 🔵 BLE — gris si no hay datos en 5 s.
  - 🔊 / 🔇 Volumen — tap = mute/unmute (sincronizado con switch del Settings).
  - 📶 Wi-Fi — tap = toggle AP on/off con el mismo modal "requiere reiniciar".
  - 🌡 Temperatura exterior — color según valor.
  - ⚙ Settings — tap = navegar a Settings, cambia a 🏠 cuando ya estás en Settings (vuelve a Live).

#### BLE Victron
- Configuración de hasta 8 dispositivos por MAC + clave AES (NVS).
- Soporte de records: 0x01 Solar Charger, 0x02 Battery Monitor, 0x03 Inverter, 0x04 DC/DC Converter, 0x05 Smart Lithium.
- Indicador BLE con timeout a gris si no llegan datos en 5 s.
- Detección automática del tipo de dispositivo y vista correspondiente.

#### Portal web
- AP `VictronConfig` automático al arrancar.
- `http://192.168.4.1` con sincronización automática de hora (script `<img>` GET → /settime, compatible con captive portals).
- Páginas con `Cache-Control: no-store` para evitar HTML cacheado.
- `/data/frigo` y `/data/bateria`: gráficos SVG con **auto-escala Y** según los valores reales y **downsample** automático cuando hay > 1500 puntos.
- En el gráfico de batería, **polígono semitransparente con el rango max-min** + línea media (avg), de modo que los picos no desaparecen aunque la línea avg suavice.
- Selector de página inicial del portal web (Keys / Logs).

#### Datalogger y persistencia
- **Frigo**: buffer circular RAM 200 entradas + CSV diario en `/sdcard/frigo/YYYY-MM-DD.csv`.
- **Batería**: 24 h en RAM (en PSRAM, 552 KB) con muestreo cada **10 s** y `avg/max/min` por intervalo. CSV diario en `/sdcard/bateria/YYYY-MM-DD.csv` con 5 columnas (`timestamp, source, milli_amps, milli_amps_max, milli_amps_min`).
- Si el CSV de hoy no existe, las páginas web sirven el más reciente del directorio.
- Flush a SD cada 60 s.
- Backup horario del epoch del sistema en NVS (`rtc_backup/epoch`) como red de seguridad si el RTC pierde la hora.

#### RTC y hora
- Zona horaria **Europe/Madrid** configurada (CET/CEST con DST automático) antes de cualquier `settimeofday`.
- El RTC almacena hora local; `mktime` la interpreta correctamente con la TZ.
- Bug del bit STOP corregido (RX8025T usa CONTROL bit 6, no CTRL0 bit 0). El bit de siglo no se escribe (asumimos siglo 21).

#### Salvapantallas
- Modos: **Atenuar** (baja brillo) o **Rotar vistas** (alterna Live → LogFrigo → LogBateria cada N min).
- En modo Rotar, al pulsar la pantalla se cierran TODOS los overlays y vuelve a la pestaña Live.
- Periodo y brillo de reposo configurables en Settings → Display.

---

### Compilación

Requisitos: ESP-IDF v5.4 o superior, target esp32p4

```bash
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
cd victron-jc1060p470c-esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

### Configuración inicial

1. Encender la pantalla — el AP WiFi VictronConfig arranca automáticamente.
2. Conectarse al WiFi VictronConfig desde el móvil o PC.
3. Abrir `http://192.168.4.1` en el navegador — la hora se sincroniza automáticamente con el RTC.
4. En Settings → Wi-Fi, seleccionar "Página inicial portal web: Keys" (por defecto).
5. Abrir de nuevo el portal — aparece la página de configuración de claves.
6. Introducir la MAC y clave AES del dispositivo Victron y guardar.
7. Una vez configurado, se puede cambiar la página inicial a "Logs" en Settings → Wi-Fi.

---

### Estado del hardware

- **SD Card** ✅ — esp_hosted ocupa el slot SDMMC 1 (GPIO matrix); la microSD usa el slot 0 con IOMUX dedicado. Requiere `esp_ldo_acquire_channel(ch=4, 3300mV)` antes de montar.
- **RTC RX8025T** ✅ — soporta pérdida de pila con backup en NVS.
- **DS18B20** ⚠️ — bus 1-Wire en GPIO26, pendiente de conexión física (resistencia pullup 4.7K a 3.3V).
- **Ventilador** ⚠️ — PWM en GPIO21 (mapeado en código), pendiente de cableado físico.

---

### Créditos

- Proyecto original: VictronSolarDisplayEsp por wytr
- Fork multi-device: victronsolardisplayesp por CamdenSutherland
- Port ESP32-P4 / Guition JC1060P470C_I + UI estilo Venus OS: Ehuntabi

---

---

## English

Port of [victronsolardisplayesp (multi-device)](https://github.com/CamdenSutherland/victronsolardisplayesp/tree/multi-device) by CamdenSutherland, itself a fork of [VictronSolarDisplayEsp](https://github.com/wytr/VictronSolarDisplayEsp) by wytr.

Ported by **[Ehuntabi](https://github.com/Ehuntabi)**.

---

### Hardware

| Component | Details |
|---|---|
| **Display** | Guition JC1060P470C_I (7", 1024x600, DSI, GT911 touch) |
| **Main SoC** | ESP32-P4 |
| **WiFi/BT SoC** | ESP32-C6 (via SDIO, managed by esp_hosted, SDMMC slot 1: CLK=18, CMD=19, D0..D3=14..17) |
| **RTC** | RX8025T — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (shared with GT911) |
| **microSD** | SDMMC slot 0 IOMUX (CLK=43, CMD=44, D0..D3=39..42), powered by internal LDO ch4 (3.3V) |
| **Temp sensors** | DS18B20 1-Wire on GPIO26 |
| **Fan control** | PWM LEDC on GPIO9, 25KHz |
| **Audio** | ES8311 codec + NS4150 amp (PA_CTRL on GPIO11). I2S MCLK=13/BCLK=12/LRCK=10/DOUT=9 |
| **RTC battery** | CR1220 |

---

### Features

#### Venus-OS-style UI
- Unified visual language inspired by Victron's Venus OS: dark cards with role-coloured borders (cyan/green/orange/red), big-number metrics with small unit, status pills, circular SOC gauges, soft drop shadows.
- **Custom PNG icons** in card headers (car battery, solar panel, DC/DC converter, house for loads), embedded as `lv_img_dsc_t` in flash.
- Reusable helpers (`main/ui/ui_card.h`): `ui_card_create`, `ui_card_set_title_img`, `ui_metric_create_compact/_large`, `ui_pill_create`, `ui_arc_soc_create`, semantic palette `UI_COLOR_*`.
- Live/Settings tab buttons hidden — navigation via discreet ⚙ icon in the bottom bar + horizontal swipe of the tabview.
- **Auto-return to Live** after 60 s of touch inactivity (skipped while screensaver is active).

#### Live views
- **Overview** (default view): vertical diagram Solar → Battery → Loads with arrows and W; central SOC arc + battery current.
- **Battery Monitor**: orange card with big SOC arc, V/A/W (font_46) and footer TTG/Consumed/Aux. Tap opens 24 h history.
- **Solar Charger**: 2 cards (green Solar + orange Battery output) with charger state pill (Bulk/Absorption/Float).
- **Default Battery**: 3 columns (cyan DC/DC, orange Battery with arc, green Solar) consolidating all sources.
- **Simple devices** (Inverter, Smart Lithium, AC Charger, etc.): single card with stacked metrics; colour and icon depending on the device type.

#### Settings
Pages with role-coloured cards and modal dialogs:
- **Frigo** (green): PWM fan control based on T_Fins (DS18B20). T_Exterior and T_Freezer are informational only.
- **Logs** (orange): 24 h battery chart (with max/min/avg downsample) and frigo temperatures chart.
- **Wi-Fi** (blue): SSID, password, on/off switch; switching shows "Wi-Fi change requires restart" modal with Cancel / Restart.
- **Display** (purple): screen and idle brightness in steps of 5; screensaver mode (Dim / Rotate views) and period.
- **Sound & alerts** (red): volume, mute switch, SoC and frigo temperature thresholds.
- **Victron Keys** (pink): MAC + AES key per device (up to 8); warning dialog on entry.
- **About** (gray): uptime, free RAM, AP IP, chip, IDF version, Reboot button with confirmation.

#### Bottom bar
- Fixed positions (defined widths, `flex_align SPACE_BETWEEN`).
- Clock + date (RTC-synced, refreshed every 30 s; instant refresh after `settime`).
- **Interactive status icons**:
  - 🔵 BLE — grey if no data in 5 s.
  - 🔊 / 🔇 Volume — tap = mute/unmute (synced with Settings switch).
  - 📶 Wi-Fi — tap = toggle AP on/off with the same "requires restart" modal.
  - 🌡 Outdoor temp — colour by value.
  - ⚙ Settings — tap = navigate to Settings, becomes 🏠 when in Settings (back to Live).

#### Victron BLE
- Up to 8 devices configurable by MAC + AES key (NVS).
- Records supported: 0x01 Solar Charger, 0x02 Battery Monitor, 0x03 Inverter, 0x04 DC/DC Converter, 0x05 Smart Lithium.
- BLE indicator with auto-grey-out if no data for 5 s.
- Automatic device-type detection and matching view.

#### Web portal
- `VictronConfig` AP starts automatically on boot.
- `http://192.168.4.1` with automatic time sync (`<img>` GET → /settime, captive-portal compatible).
- All pages with `Cache-Control: no-store` to avoid stale HTML.
- `/data/frigo` and `/data/bateria`: SVG charts with **Y-axis auto-scale** based on real values and **automatic downsample** when > 1500 points.
- Battery chart shows a **semitransparent max-min envelope polygon** + average line, so peaks remain visible even when the average smooths.
- Web portal start page selector (Keys / Logs).

#### Datalogger and persistence
- **Frigo**: 200-entry RAM ring buffer + daily CSV at `/sdcard/frigo/YYYY-MM-DD.csv`.
- **Battery**: 24 h in RAM (in PSRAM, 552 KB) sampling every **10 s** with `avg/max/min` per interval. Daily CSV at `/sdcard/bateria/YYYY-MM-DD.csv` with 5 columns (`timestamp, source, milli_amps, milli_amps_max, milli_amps_min`).
- If today's CSV doesn't exist, the web pages serve the most recent file in the directory.
- SD flush every 60 s.
- Hourly backup of the system epoch in NVS (`rtc_backup/epoch`) as a safety net if the RTC loses time.

#### RTC and time
- **Europe/Madrid** timezone configured (CET/CEST with automatic DST) before any `settimeofday`.
- The RTC stores local time; `mktime` interprets it correctly with the TZ.
- STOP-bit bug fixed (RX8025T uses CONTROL bit 6, not CTRL0 bit 0). Century bit not written (assumes 21st century).

#### Screensaver
- Modes: **Dim** (lower brightness) or **Rotate views** (cycle Live → FrigoLog → BatteryLog every N min).
- In Rotate mode, tapping the screen closes ALL overlays and returns to the Live tab.
- Idle brightness and period configurable in Settings → Display.

---

### Build

Requirements: ESP-IDF v5.4 or later, target esp32p4

```bash
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
cd victron-jc1060p470c-esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

### First-time setup

1. Power on the display — the VictronConfig WiFi AP starts automatically.
2. Connect to the VictronConfig WiFi from your phone or PC.
3. Open `http://192.168.4.1` in your browser — time is automatically synced to the RTC.
4. In Settings → Wi-Fi, select "Web portal start page: Keys" (default).
5. Open the portal again — the key configuration page appears.
6. Enter your Victron device MAC and AES encryption key and save.
7. Once configured, you can change the start page to "Logs" in Settings → Wi-Fi.

---

### Hardware status

- **SD Card** ✅ — esp_hosted uses SDMMC slot 1 (GPIO matrix); microSD uses slot 0 with dedicated IOMUX. Requires `esp_ldo_acquire_channel(ch=4, 3300mV)` before mount.
- **RTC RX8025T** ✅ — survives battery loss with NVS backup.
- **DS18B20** ⚠️ — 1-Wire bus on GPIO26, pending physical connection (4.7K pullup to 3.3V).
- **Fan** ⚠️ — PWM on GPIO21 (mapped in code), pending physical wiring.

---

### Credits

- Original project: VictronSolarDisplayEsp by wytr
- Multi-device fork: victronsolardisplayesp by CamdenSutherland
- ESP32-P4 / Guition JC1060P470C_I port + Venus-OS-style UI: Ehuntabi

This project keeps the same license as the original. See LICENSE.
