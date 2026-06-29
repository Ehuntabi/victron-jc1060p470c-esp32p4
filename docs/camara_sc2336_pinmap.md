# Pinmap camara SC2336 — JC1060P470C_I_W (ESP32-P4)

Para el Hito 1 (camara + auto-brillo). Valores confirmados de fuentes publicas + a verificar
en bring-up (Fase 1).

## Sensor
- Modelo: **SC2336** (2 MP). Chip ID: **0x5602**.
- Salida nativa: 1920x1080 RAW10 (1936x1096 area activa). Para auto-brillo se usara la
  resolucion mas baja util con componente de luma (Y) accesible.

## SCCB (control I2C del sensor)
- Direccion: **0x36**.
- Bus: **I2C bus 1**, **SDA = GPIO 7**, **SCL = GPIO 8**.
- NOTA: es el **mismo bus I2C interno** que ya usan el touch GT911 y el RTC RX8025T
  (`BSP_I2C_SDA=GPIO7`, `BSP_I2C_SCL=GPIO8`). 0x36 no colisiona con esas direcciones.
  -> El servicio `camera` debe **reusar ese bus** (no crear otro), o coordinar con el driver I2C existente.

## MIPI-CSI
- **Data lanes: 2**.
- **Lane bit rate: 800 Mbps/carril** (link 400 MHz DDR; 1600 Mbps total).
- **MCLK: 19.2 MHz**.
- Error tipico de init mal configurada a vigilar en el log: **0x102**.

## Reset / Power-down (PENDIENTE confirmar del esquematico del board)
- Valor generico visto en foros: GPIO 43/44, **pero en este board GPIO 43/44 pueden estar
  asignados al SDIO del ESP32-C6** -> conflicto. La camara integrada del board esta cableada a
  pines que NO chocan con SDIO: hay que sacar los GPIO reales de **reset** y **PWDN** del
  esquematico `docs/JC1060P470C_I_W_Y-V1.0_esquematico.pdf` (seccion CSI_Camera / FPC).
- Verificacion definitiva: en Fase 1, si `camera_init` detecta **chip id 0x5602** por SCCB, el
  reset/PWDN y el MCLK estan bien.

## esp_video / esp_cam_sensor
- `esp_video` v2.2.0 (idf >= 5.4, compatible con 5.4.4). Driver: `esp_cam_sensor` -> SC2336.
- Acceso por API V4L2 (open device, set format a baja resolucion, dequeue/enqueue buffers).

## Referencias
- Tasmota discussion ESP32-P4 CSI CAM testing (config SCCB/lanes/MCLK).
- Issue espressif/esp-video-components #53 (bring-up SC2336 en P4).
- Esquematico del board (reset/PWDN reales).
