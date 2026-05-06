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
| **SoC WiFi/BT** | ESP32-C6 (via SDIO, gestionado por esp_hosted) |
| **RTC** | RX8130CE — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (compartido con GT911) |
| **Tarjeta SD** | Slot microSD integrado — GPIO36=CLK, GPIO35=CMD, GPIO37=D0 *(ver problemas conocidos)* |
| **Sensores temperatura** | DS18B20 1-Wire en GPIO26 |
| **Control ventilador** | PWM LEDC en GPIO9, 25KHz |
| **Batería RTC** | CR1220 |

---

### Funcionalidades

- Display DSI 1024x600 con touch capacitivo GT911
- WiFi AP via ESP32-C6 con esp_hosted sobre SDIO
- BLE Victron Energy — monitor de baterias y cargadores solares
- Portal web de configuracion en http://192.168.4.1
- Sync de hora automatica al abrir el portal web (JS a RTC RX8130)
- Reloj y fecha en pantalla (esquina inferior izquierda)
- Indicador de estado BLE en pantalla (centro inferior)
- Tab Frigo: control de temperatura optima en aletas traseras de frigo trivalente mediante sensor DS18B20 en aletas 
  y control PWM del ventilador. T_Exterior y T_Congelador son informativas
- Datalogger en RAM: buffer circular 200 entradas, descargable como CSV desde /logs
- Selector de pagina inicial del portal web (Keys / Logs)
- Pagina About en Settings con version y creditos
- TWDT con panic activado
- Tarjeta SD pendiente fix Espressif (issue ESP-IDF #17889)
- DS18B20 pendiente conexion fisica en GPIO26

---

### Compilacion

Requisitos: ESP-IDF v5.4 o superior, target esp32p4

```bash
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
cd victron-jc1060p470c-esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

---

### Configuracion inicial

1. Encender la pantalla — el AP WiFi VictronConfig arranca automaticamente
2. Conectarse al WiFi VictronConfig desde el movil o PC
3. Abrir http://192.168.4.1 en el navegador
4. La hora se sincroniza automaticamente con el RTC
5. En Settings → Wi-Fi, seleccionar "Pagina inicial portal web: Keys" (por defecto)
6. Abrir de nuevo http://192.168.4.1 — aparece la pagina de configuracion de claves
7. Introducir la MAC y clave AES del dispositivo Victron y guardar
8. Una vez configurado, se puede cambiar la pagina inicial a "Logs" en Settings → Wi-Fi
   para que el portal muestre directamente los datos del datalogger
---

### Datalogger

El sistema registra cada 5 minutos: timestamp, T_Aletas, T_Congelador, T_Exterior y porcentaje ventilador. 
Buffer circular de 200 entradas en RAM (~16 horas). Para descargar el CSV, cambiar la pagina inicial a "Logs" en 
Settings → Wi-Fi y abrir http://192.168.4.1
Cuando el bug de la SD este resuelto por Espressif, los logs se migraran a tarjeta SD.

---

### RTC RX8130

El chip es RX8130, no RX8025T como indica el esquematico de Guition.
- Direccion I2C: 0x32
- Bus compartido con GT911: GPIO7=SDA, GPIO8=SCL
- Sin pila CR1220 pierde la hora al reiniciar
- La hora se sincroniza al abrir el portal web

---

### Problemas conocidos

**SD Card — ESP_ERR_TIMEOUT**
El ESP32-P4 tiene un unico controlador SDMMC, reclamado por esp_hosted para SDIO. Confirmado como bug en espressif/esp-idf issue #17889. Estado: Selected for Development.

**DS18B20 — Sin conexion fisica**
Bus 1-Wire configurado en GPIO26. Conectar con resistencia pullup de 4.7K a 3.3V.

---

### Creditos

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
| **WiFi/BT SoC** | ESP32-C6 (via SDIO, managed by esp_hosted) |
| **RTC** | RX8130CE — I2C addr 0x32, bus GPIO7=SDA / GPIO8=SCL (shared with GT911) |
| **SD Card** | Onboard microSD slot — GPIO36=CLK, GPIO35=CMD, GPIO37=D0 *(see known issues)* |
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
- BLE status indicator on screen (bottom center)
- Fridge tab: optimal temperature control on the rear fins of a trivalent fridge via DS18B20 sensor on fins and PWM fan      control. T_Exterior and T_Freezer are informational only
- RAM datalogger: 200-entry circular buffer, downloadable as CSV from /logs
- Web portal start page selector (Keys / Logs)
- About page in Settings with version and credits
- TWDT with panic enabled
- SD card pending Espressif fix (ESP-IDF issue #17889)
- DS18B20 pending physical connection on GPIO26

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
   so the portal shows the datalogger directly

---

### Datalogger

Logs every 5 minutes: timestamp, T_Fins, T_Freezer, T_Exterior and fan percentage. 200-entry circular buffer in RAM (~16 hours). To download the CSV, change the start page to "Logs" in Settings → Wi-Fi and open http://192.168.4.1

Once the SD card bug is fixed by Espressif, logs will migrate to SD card.

---

### RTC RX8130

The chip is RX8130, not RX8025T as stated in the Guition schematic.
- I2C address: 0x32
- Shared bus with GT911: GPIO7=SDA, GPIO8=SCL
- Without CR1220 battery, time is lost on power cycle
- Time is synced when opening the web portal

---

### Known Issues

**SD Card — ESP_ERR_TIMEOUT**
The ESP32-P4 has a single SDMMC controller, claimed by esp_hosted for SDIO. Confirmed bug in espressif/esp-idf issue #17889. Status: Selected for Development.

**DS18B20 — Not physically connected**
1-Wire bus configured on GPIO26. Connect with 4.7K pullup resistor to 3.3V.

---

### Credits

- Original project: VictronSolarDisplayEsp by wytr
- Multi-device fork: victronsolardisplayesp by CamdenSutherland
- ESP32-P4 / Guition JC1060P470C_I port: Ehuntabi

This project maintains the same license as the original. See LICENSE.
