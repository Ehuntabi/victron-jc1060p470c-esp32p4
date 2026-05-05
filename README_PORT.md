# Port: victronsolardisplayesp → Guition JC1060P470C_I (7", 1024×600)

## Hardware de destino

| Parámetro          | Valor                                      |
|--------------------|--------------------------------------------|
| SoC                | ESP32-P4 (dual-core 400 MHz)               |
| PSRAM              | 32 MB integrada                            |
| Flash              | 16 MB                                      |
| Panel              | 7.0" IPS — QD070AS01-1                     |
| Controlador LCD    | JD9165BA (Jadard/Himax)                    |
| Interfaz LCD       | MIPI-DSI, 2 data lanes, 750 Mbps/lane      |
| Resolución         | 1024 × 600 px                              |
| Pixel clock        | 51.2 MHz (≈ 52 MHz en IDF)                 |
| Touch              | GT911 capacitivo (I2C)                     |
| Conectividad       | WiFi + Bluetooth (integrados en ESP32-P4)  |

---

## Resumen de cambios respecto al proyecto original

| Aspecto           | Original (JC3248W535)           | Este port (JC1060P470C_I)           |
|-------------------|---------------------------------|--------------------------------------|
| SoC               | ESP32-S3                        | **ESP32-P4**                        |
| Bus LCD           | QSPI (SPI2_HOST)                | **MIPI-DSI** (2 lanes)              |
| Controlador LCD   | AXS15231B                       | **JD9165BA**                        |
| Touch             | AXS15231B (I2C)                 | **GT911** (I2C)                     |
| Resolución        | 320 × 480                       | **1024 × 600**                      |
| I2C IDF API       | `i2c_param_config` (legacy)     | `i2c_new_master_bus` (IDF ≥ 5.3)  |
| LVGL port         | `lvgl_port_add_disp`            | `lvgl_port_add_disp_dsi`            |
| Init secuencia    | AXS15231B custom                | **JD9165BA desde dtsi oficial**     |

**main.c, ui.c y todos los módulos de lógica (victron_ble, config_server…)
no necesitan ningún cambio.** La API BSP pública es idéntica.

---

## Ficheros entregados en este port

```
victron-jc1060/
├── CMakeLists.txt            ← Target esp32p4, mismo nombre de proyecto
├── sdkconfig.defaults        ← CPU 360 MHz, 32 MB PSRAM, 16 MB Flash
└── main/
    ├── CMakeLists.txt        ← Dependencias MIPI-DSI / JD9165 / GT911
    ├── idf_component.yml     ← esp_lcd_jd9165 + gt911 + esp_lvgl_port
    ├── display.h             ← Pines, resolución y timings JD9165BA
    ├── esp_bsp.h             ← API pública (idéntica al original)
    └── esp_bsp.c             ← Implementación MIPI-DSI completa
```

---

## Instrucciones de integración

### 1. Copiar los ficheros del port sobre el proyecto original

```bash
# Partimos de una copia del proyecto original
cp -r victronsolardisplayesp-multi-device/  mi_proyecto_jc1060/

# Sustituir ficheros BSP en main/
cp display.h esp_bsp.h esp_bsp.c CMakeLists.txt idf_component.yml \
   mi_proyecto_jc1060/main/

# Sustituir ficheros raíz
cp CMakeLists.txt sdkconfig.defaults  mi_proyecto_jc1060/

# Eliminar el lv_port.c/h original (sustituido por esp_lvgl_port)
rm mi_proyecto_jc1060/main/lv_port.c  mi_proyecto_jc1060/main/lv_port.h
```

### 2. Verificar los GPIO en `display.h`

Comprueba con el esquemático de tu módulo que los pines coinciden:

```c
#define BSP_LCD_BACKLIGHT   (GPIO_NUM_23)   // PWM retroiluminación
#define BSP_LCD_RST         (GPIO_NUM_0)    // Reset panel (o GPIO_NUM_NC)
#define BSP_I2C_SDA         (GPIO_NUM_7)    // I2C touch GT911
#define BSP_I2C_SCL         (GPIO_NUM_8)    // I2C touch GT911
#define BSP_LCD_TOUCH_INT   (GPIO_NUM_NC)   // INT touch (si está cableado)
#define BSP_LCD_TOUCH_RST   (GPIO_NUM_NC)   // RST touch (si está cableado)
```

### 3. Eliminar referencias a lv_port.c del original

Si `main/CMakeLists.txt` original tenía referencias manuales a `lv_port.c`,
ya están eliminadas: el nuevo `CMakeLists.txt` usa el glob `*.c` y `esp_lvgl_port`
como dependencia.

### 4. Compilar y flashear

```bash
cd mi_proyecto_jc1060
idf.py set-target esp32p4
idf.py reconfigure          # aplica sdkconfig.defaults
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Notas sobre la rotación

El panel es **landscape nativo** (1024×600).  
El proyecto original usaba rotación 90° para una pantalla portrait de 320×480.  
Tienes dos opciones:

**Opción A — Sin rotación (recomendado para este panel):**
```c
// en main.c
#define LVGL_PORT_ROTATION_DEGREE 0
// .rotate = LV_DISP_ROT_NONE
```
La UI verá 1024×600 y deberás adaptar los layouts.

**Opción B — Mantener 90° (mínimo cambio):**
```c
#define LVGL_PORT_ROTATION_DEGREE 90
// LVGL ve 600×1024 lógico
```
La UI existente funcionará sin cambios, aunque la relación de aspecto cambia.

---

## Timings verificados

Los timings de `display.h` provienen del fichero oficial del fabricante:

```
MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_2lane.dtsi

HSYNC pulse=24  HBP=136  HFP=160
VSYNC pulse=2   VBP=21   VFP=12
DOTCLK=51.2 MHz → 52 MHz
Lanes=2  @  750 Mbps/lane
```

La secuencia de init en `esp_bsp.c` (`s_lcd_init_cmds[]`) es una
transcripción literal de ese fichero dtsi, incluyendo los delays de
120 ms (Sleep Out) y 20 ms (Display On).

---

## Solución de problemas habituales

| Síntoma                        | Causa probable                   | Solución                                      |
|--------------------------------|----------------------------------|-----------------------------------------------|
| Pantalla en blanco             | GPIO reset incorrecto            | Ajustar `BSP_LCD_RST` en `display.h`          |
| Imagen desplazada H            | HBP incorrecto                   | Cambiar `BSP_LCD_MIPI_HSYNC_BACK_PORCH` a 160 |
| Touch no responde              | I2C SDA/SCL cambiados            | Ajustar `BSP_I2C_SDA/SCL` en `display.h`      |
| Error "LDO channel"            | LDO canal incorrecto en P4       | Verificar `BSP_MIPI_DSI_PHY_PWR_LDO_CHAN`     |
| Pantallazo blanco al init      | Init seq. incorrecta             | Verificar versión del panel con Guition        |
| `esp_lcd_jd9165` no encontrado | Componente no en registry IDF    | Copiar manualmente desde el Demo_IDF.zip       |
