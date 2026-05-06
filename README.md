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
| **SoC WiFi/BT** | ESP32-C6 (via SDIO, gestionado por esp_hosted, SDMMC slot 1: CLK=18, CMD=19, D0..D3=14..17) |
| **RTC** | RX8130CE — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (compartido con GT911) |
| **Tarjeta microSD** | SDMMC slot 0 IOMUX (CLK=43, CMD=44, D0..D3=39..42), alimentación por LDO interno ch4 (3.3V) |
| **Sensores temperatura** | DS18B20 1-Wire en GPIO26 |
| **Control ventilador** | PWM LEDC en GPIO9, 25KHz |
| **Batería RTC** | CR1220 |

---

### Funcionalidades

- Display DSI 1024x600 con touch capacitivo GT911
- WiFi AP via ESP32-C6 con esp_hosted sobre SDIO
- BLE Victron Energy — monitor de baterías y cargadores solares
- Portal web de configuración en http://192.168.4.1
- Sync de hora automática al abrir el portal web (JS a RTC RX8130)
- Reloj y fecha en pantalla (esquina inferior izquierda)
- Indicador BLE con timeout a gris si no llegan datos en 5s
- Tab Live: vistas dinámicas según el dispositivo Victron detectado (BMV, MPPT, Orion, AC charger, etc.)
- **Histórico de batería 24h**: tap en la vista Battery Monitor abre pantalla modal con chart multi-fuente
  (Battery Monitor, Solar, Orion XS, AC Charger), totales de carga/descarga (Ah) y eje X con hora real
- **Tab Frigo dentro de Settings**: control PWM del ventilador en función de T_Aletas (DS18B20).
  T_Exterior y T_Congelador informativos
- **Settings reorganizado** con aspecto de botones: Frigo, Wi-Fi, Display, Victron Keys, About
- About con info dinámica: uptime, RAM libre, IP del AP, chip, versión IDF, y botón Reboot con confirmación
- Datalogger frigo: buffer circular RAM 200 entradas + CSV diario en `/sdcard/frigo/YYYY-MM-DD.csv`
- Datalogger batería: histórico 24h en RAM + CSV diario en `/sdcard/bateria/YYYY-MM-DD.csv`
- Persistencia NVS del histórico batería cada 15 min (sobrevive reinicios)
- Selector de página inicial del portal web (Keys / Logs)
- TWDT con panic activado

---

### Compilación

Requisitos: ESP-IDF v5.4 o superior, target esp32p4

```bash
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
cd victron-jc1060p470c-esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

---

### Configuración inicial

1. Encender la pantalla — el AP WiFi VictronConfig arranca automáticamente
2. Conectarse al WiFi VictronConfig desde el móvil o PC
3. Abrir http://192.168.4.1 en el navegador
4. La hora se sincroniza automáticamente con el RTC
5. En Settings → Wi-Fi, seleccionar "Página inicial portal web: Keys" (por defecto)
6. Abrir de nuevo http://192.168.4.1 — aparece la página de configuración de claves
7. Introducir la MAC y clave AES del dispositivo Victron y guardar
8. Una vez configurado, se puede cambiar la página inicial a "Logs" en Settings → Wi-Fi

---

### Datalogger y SD

Estructura en SD:

```
/sdcard/
  frigo/
    2026-05-06.csv      timestamp, T_Aletas, T_Congelador, T_Exterior, fan_pct
    2026-05-07.csv
  bateria/
    2026-05-06.csv      timestamp, source, milli_amps
```

- Volcado a SD cada 60 s (mejor vida útil de la tarjeta)
- Si la SD no está montada, los datos siguen disponibles en RAM
- Los CSV se descargan también desde el portal web (datalogger frigo)

**Nota sobre SD**: la microSD usa el SDMMC slot 0 del ESP32-P4 (IOMUX dedicado), independiente del slot 1 que usa esp_hosted para WiFi. Es necesario activar el LDO interno ch4 (3.3V) para alimentar TF_VCC. FATFS está configurado con LFN heap mode para nombres largos.

---

### Histórico de batería

Pantalla modal accesible al pulsar la vista Battery Monitor en el tab Live:

- 4 series simultáneas (Battery Monitor / Solar Charger / Orion XS / AC Charger), cada una con su color
- Eje X: hora real (HH:MM) tomada del RTC
- Eje Y: corriente en amperios (-40 a +40 A), positivo=carga, negativo=descarga
- Totales acumulados (Ah cargados / descargados) por cada fuente
- Buffer 24h con muestreo cada 3 min (480 puntos)
- Persistencia en NVS cada 15 min — sobrevive reinicios y flasheos `idf.py flash`
- Volcado a `/sdcard/bateria/YYYY-MM-DD.csv` cada 60 s

---

### RTC RX8130

El chip es RX8130, no RX8025T como indica el esquemático de Guition.
- Dirección I2C: 0x32
- Bus compartido con GT911: GPIO7=SDA, GPIO8=SCL
- Sin pila CR1220 pierde la hora al reiniciar
- La hora se sincroniza al abrir el portal web

---

### Estado del hardware

**SD Card — Funcionando** ✅
Resuelto: el ESP32-P4 tiene 2 slots SDMMC. esp_hosted ocupa el slot 1 (GPIO matrix); la microSD usa el slot 0 con IOMUX dedicado, sin conflicto. Requiere activar `esp_ldo_acquire_channel(ch=4, 3300mV)` antes de montar.

**DS18B20 — Sin conexión física**
Bus 1-Wire configurado en GPIO26. Conectar con resistencia pullup de 4.7K a 3.3V.

---

### Créditos

- Proyecto original: VictronSolarDisplayEsp por wytr
- Fork multi-device: victronsolardisplayesp por CamdenSutherland
- Port ESP32-P4 / Guition JC1060P470C_I: Ehuntabi

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
| **RTC** | RX8130CE — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (shared with GT911) |
| **microSD** | SDMMC slot 0 IOMUX (CLK=43, CMD=44, D0..D3=39..42), powered by internal LDO ch4 (3.3V) |
| **Temp sensors** | DS18B20 1-Wire on GPIO26 |
| **Fan control** | PWM LEDC on GPIO9, 25KHz |
| **RTC battery** | CR1220 |

---

### Features

- DSI 1024x600 display with GT911 capacitive touch
- WiFi AP via ESP32-C6 with esp_hosted over SDIO
- Victron Energy BLE — battery monitor and solar charger
- Web configuration portal at http://192.168.4.1
- Automatic time sync when opening web portal (JS to RTC RX8130)
- Clock and date on screen (bottom left)
- BLE status indicator with auto-grey-out if no data for 5s
- Live tab: dynamic views based on detected Victron device (BMV, MPPT, Orion, AC charger, etc.)
- **24h battery history**: tap the Battery Monitor view to open a modal chart with multiple sources
  (Battery Monitor, Solar, Orion XS, AC Charger), charge/discharge totals (Ah) and X-axis with real time
- **Frigo tab inside Settings**: PWM fan control based on T_Fins (DS18B20).
  T_Exterior and T_Freezer are informational only
- **Restyled Settings menu** with button look: Frigo, Wi-Fi, Display, Victron Keys, About
- About page with dynamic info: uptime, free RAM, AP IP, chip, IDF version, and Reboot button with confirmation
- Frigo datalogger: 200-entry RAM circular buffer + daily CSV at `/sdcard/frigo/YYYY-MM-DD.csv`
- Battery datalogger: 24h RAM history + daily CSV at `/sdcard/bateria/YYYY-MM-DD.csv`
- NVS persistence of battery history every 15 min (survives reboots)
- Web portal start page selector (Keys / Logs)
- TWDT with panic enabled

---

### Build

Requirements: ESP-IDF v5.4 or later, target esp32p4

```bash
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
cd victron-jc1060p470c-esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

---

### First time setup

1. Power on the display — the VictronConfig WiFi AP starts automatically
2. Connect to the VictronConfig WiFi from your phone or PC
3. Open http://192.168.4.1 in your browser
4. Device time is automatically synced to the RTC
5. In Settings → Wi-Fi, select "Web portal start page: Keys" (default)
6. Open http://192.168.4.1 again — the key configuration page appears
7. Enter your Victron device MAC address and AES encryption key and save
8. Once configured, you can change the start page to "Logs" in Settings → Wi-Fi

---

### Datalogger and SD

SD layout:

```
/sdcard/
  frigo/
    2026-05-06.csv      timestamp, T_Fins, T_Freezer, T_Exterior, fan_pct
    2026-05-07.csv
  bateria/
    2026-05-06.csv      timestamp, source, milli_amps
```

- Flushed to SD every 60s (better SD lifespan)
- If SD is not mounted, data still available in RAM
- Frigo CSV is also downloadable via the web portal

**SD note**: the microSD uses ESP32-P4 SDMMC slot 0 (dedicated IOMUX), independent from slot 1 used by esp_hosted for WiFi. Internal LDO ch4 (3.3V) must be enabled to power TF_VCC. FATFS is configured with LFN heap mode for long filenames.

---

### Battery history

Modal screen accessible by tapping the Battery Monitor view in the Live tab:

- 4 simultaneous series (Battery Monitor / Solar Charger / Orion XS / AC Charger), each with its own color
- X-axis: real time (HH:MM) from the RTC
- Y-axis: current in amperes (-40 to +40 A), positive=charge, negative=discharge
- Accumulated totals (Ah charged / discharged) for each source
- 24h buffer with sampling every 3 min (480 points)
- NVS persistence every 15 min — survives reboots and `idf.py flash`
- Flushed to `/sdcard/bateria/YYYY-MM-DD.csv` every 60s

---

### RTC RX8130

The chip is RX8130, not RX8025T as stated in the Guition schematic.
- I2C address: 0x32
- Shared bus with GT911: GPIO7=SDA, GPIO8=SCL
- Without CR1220 battery, time is lost on power cycle
- Time is synced when opening the web portal

---

### Hardware status

**SD Card — Working** ✅
Solved: ESP32-P4 has 2 SDMMC slots. esp_hosted uses slot 1 (GPIO matrix); the microSD uses slot 0 with dedicated IOMUX, no conflict. Requires `esp_ldo_acquire_channel(ch=4, 3300mV)` before mount.

**DS18B20 — Not physically connected**
1-Wire bus configured on GPIO26. Connect with 4.7K pullup resistor to 3.3V.

---

### Credits

- Original project: VictronSolarDisplayEsp by wytr
- Multi-device fork: victronsolardisplayesp by CamdenSutherland
- ESP32-P4 / Guition JC1060P470C_I port: Ehuntabi

This project maintains the same license as the original. See LICENSE.
