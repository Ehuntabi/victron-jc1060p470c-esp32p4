# VictronSolarDisplay — Port para Guition JC1060P470C_I (ESP32-P4)

Port del proyecto [victronsolardisplayesp (multi-device)](https://github.com/CamdenSutherland/victronsolardisplayesp/tree/multi-device) de CamdenSutherland, que a su vez es un fork de [VictronSolarDisplayEsp](https://github.com/wytr/VictronSolarDisplayEsp) de wytr.

## Descripción

Monitor de sistemas Victron Energy con display táctil de 7 pulgadas, control de frigorífico y logging de datos a tarjeta SD.

El proyecto original estaba desarrollado para una pantalla **Guition JC3248W535 (3.5", ESP32-S3)**. Este port lo adapta a la pantalla **Guition JC1060P470C_I (7", 1024x600, ESP32-P4)**.

## Hardware

- **Pantalla**: Guition JC1060P470C_I (1024x600, DSI, touch GT911)
- **SoC principal**: ESP32-P4
- **SoC WiFi/BT**: ESP32-C6 (comunicación via SDIO, gestionado por esp_hosted)
- **RTC**: RX8025T (I2C, GPIO10=SCL, GPIO12=SDA) — *ver problemas conocidos*
- **Tarjeta SD**: slot microSD integrado (GPIO36=CLK, GPIO35=CMD, GPIO37=D0) — *ver problemas conocidos*
- **Sensores temperatura**: DS18B20 (1-Wire, GPIO26)
- **Control ventilador**: PWM LEDC (GPIO9, 25KHz)
- **Batería RTC**: CR1220

## Funcionalidades implementadas

- ✅ Display DSI 1024x600 con touch capacitivo GT911
- ✅ WiFi via ESP32-C6 (esp_hosted sobre SDIO)
- ✅ Monitorización Victron Energy via BLE (baterías, cargadores solares)
- ✅ Servidor de configuración WiFi (portal captivo)
- ✅ Tab Frigo: lectura DS18B20, control PWM ventilador, histéresis configurable
- ✅ Asignación de sensores DS18B20 por dirección
- ✅ Umbrales T_min/T_max configurables (30-50°C, paso 5°C)
- ✅ Persistencia de configuración en NVS
- ✅ Logger CSV en SD (cada 5 minutos, archivo diario LOG_YYYYMMDD.csv)
- ✅ RTC RX8025T para timestamps reales en logs

## Problemas conocidos / Ayuda necesaria

### 🔴 SD Card — ESP_ERR_TIMEOUT

La tarjeta SD no monta correctamente. El problema parece estar relacionado con el conflicto entre el driver `esp_hosted` (que inicializa el host SDMMC completo para la comunicación con el ESP32-C6 via SDIO) y el acceso a la tarjeta SD física.

**Configuración hardware**: CLK=GPIO36, CMD=GPIO35, D0=GPIO37, D1=GPIO38, D2=GPIO39, D3=GPIO40  
**Alimentación**: TF_VCC controlada por Q1 (AO3401 PMOS) cuyo gate viene de ESP_LDO_VO4  
**Intentado**: modo SDMMC slot0, modo SDMMC slot1, modo SPI (SPI2, SPI3), con/sin sd_pwr_ctrl_new_on_chip_ldo  
**Error**: `sdmmc_card_init failed (0x107)` / `ESP_ERR_TIMEOUT` en todos los modos  


**Actualización**: Confirmado como bug de ESP-IDF — issue #17889 en espressif/esp-idf. 
El ESP32-P4 tiene un único controlador SDMMC que es reclamado por esp_hosted para SDIO.
Pendiente de fix por Espressif. Estado: "Selected for Development".

¿Alguien ha conseguido usar la SD en esta placa junto con esp_hosted?

### ✅ RTC — RESUELTO

El chip es **RX8130** (no RX8025T como indica el esquemático).  
- Dirección I2C: **0x32**  
- Bus: **I2C_NUM_1** (GPIO7=SDA, GPIO8=SCL) — compartido con el touch GT911  
- La hora se sincroniza automáticamente al abrir el portal web desde cualquier dispositivo  
- Sin pila CR1220 pierde la hora al reiniciar — se recomienda instalar pila **CR1220**

### 🟡 DS18B20 — Sin sensores detectados

Los sensores DS18B20 aún no están físicamente conectados. El bus 1-Wire está configurado en GPIO26.

## Estructura del proyecto

```
victron/
├── main/
│   ├── main.c                    # Punto de entrada
│   ├── ui.c / ui.h               # Gestión UI principal (LVGL)
│   ├── esp_bsp.c / esp_bsp.h     # Board Support Package ESP32-P4
│   ├── display.h                 # Definiciones display y pines
│   └── ui/
│       ├── frigo_panel.c/h       # Panel control frigorífico
│       ├── settings_panel.c/h    # Panel configuración
│       └── ...
├── components/
│   ├── frigo/                    # DS18B20 + PWM ventilador
│   ├── rtc_rx8025t/              # Driver RTC RX8025T
│   ├── datalogger/               # Logger SD CSV
│   ├── victron_ble/              # Comunicación BLE Victron
│   └── config_storage/           # Almacenamiento configuración NVS
└── CMakeLists.txt
```

## Compilación

### Requisitos

- ESP-IDF v5.4 o superior
- Target: esp32p4

### Pasos

```bash
git clone <este-repo>
cd victron
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Créditos y licencia

- Proyecto original: [VictronSolarDisplayEsp](https://github.com/wytr/VictronSolarDisplayEsp) por **wytr**
- Fork multi-device: [victronsolardisplayesp/tree/multi-device](https://github.com/CamdenSutherland/victronsolardisplayesp/tree/multi-device) por **CamdenSutherland**
- Este port: adaptación a ESP32-P4 / Guition JC1060P470C_I

Este proyecto mantiene la misma licencia que el proyecto original. Ver [LICENSE](LICENSE).
