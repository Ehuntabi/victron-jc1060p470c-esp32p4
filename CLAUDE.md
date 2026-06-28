# VictronSolarDisplay - Guition JC1060P470C_I

Repo: github.com/Ehuntabi/victron-jc1060p470c-esp32p4

> Antes de cualquier trabajo de código no trivial aplicar
> [`andrej-karpathy-skills:karpathy-guidelines`](https://github.com/multica-ai/andrej-karpathy-skills):
> Think Before Coding · Simplicity First · Surgical Changes · Goal-Driven Execution.

## Hardware
- ESP32-P4 (principal) + ESP32-C6 vía SDIO (Wi-Fi/BT con esp_hosted)
- Display DSI 1024x600 (panel JD9165BA)
- Touch GT911, RTC RX8130, microSD slot 0 IOMUX
- Codec audio ES8311 + amplificador NS4150 (GPIO11 PA_CTRL)
- Pines I2S: MCLK=GPIO13, BCLK=GPIO12, LRCK=GPIO10, DOUT=GPIO9
- Ventilador frigo PWM en GPIO5 (JP1 pin 15)
- Bus 1-Wire DS18B20 en GPIO4 (JP1 pin 13, pullup 4.7K a 3.3V)

## Stack
- ESP-IDF v5.4.4
- LVGL para la UI
- Workspace: ~/victron

## Comandos habituales
- Compilar: `idf.py build`
- Flashear: `idf.py -p /dev/ttyACM1 flash`
- Monitor: `idf.py -p /dev/ttyACM1 monitor`
- Si pierdes el entorno IDF: `. ~/esp/esp-idf/export.sh`

## Convenciones del proyecto
- Estética card-based aplicada en Settings (cada página con su color de borde)
- Textos en español, sin emojis (excepto símbolos LV_SYMBOL_*)
- Persistencia con NVS por componente (namespace propio)
- Logs sin acentos para evitar problemas de codificación

## Estructura clave
- main/main.c: app_main, init audio/alerts/RTC
- main/ui.c: tabview, barra inferior, screensaver, hora
- main/ui/settings_panel.c: todas las páginas de Settings
- main/ui/frigo_panel.c: panel del frigorífico
- components/audio_es8311/: codec con jingles BOOT_OK/CRITICAL/WARNING/CONFIRM
- components/alerts/: thresholds NVS (freezer/SoC)
- components/config_storage/: persistencia general (Wi-Fi, screensaver, etc.)

## Pendientes activos
1. Victron Keys 2 columnas (intento alternativo, el anterior no renderizaba textos)
2. DS18B20 físicos (cuando se conecten)
3. Ventilador GPIO21 (cuando esté cableado)
