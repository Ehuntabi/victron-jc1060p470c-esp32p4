/*
 * display.h — BSP display definitions para Guition JC1060P470C_I
 *
 * Panel:       7.0" IPS  QD070AS01-1
 * Controlador: JD9165BA  (Jadard/Himax)
 * Touch:       GT911 capacitivo (I2C)
 * Host SoC:    ESP32-P4  (dual-core 400 MHz, 32 MB PSRAM, 16 MB Flash)
 * Resolución:  1024 × 600 px
 * Interfaz:    MIPI-DSI, 2 data lanes
 *
 * ── Timings extraídos del fichero oficial del fabricante ──────────────────
 *   MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_2lane.dtsi
 *
 *   JDEVB_RSOX   = 1024       H size
 *   JDEVB_RSOY   = 600        V size
 *   JDEVB_VS     = 2          VSYNC pulse width
 *   JDEVB_VBP    = 21         VSYNC back porch
 *   JDEVB_VFP    = 12         VSYNC front porch
 *   JDEVB_HS     = 24         HSYNC pulse width
 *   JDEVB_HBP    = 136        HSYNC back porch
 *   JDEVB_HFP    = 160        HSYNC front porch
 *   JDEVB_DOTCLK = 51.2 MHz   → redondeado a 52 MHz
 *   Lanes        = 2          (reg 0x0B = 0x11)
 *
 * PORT desde victronsolardisplayesp-multi-device
 *   Original: JC3248W535 / QSPI / AXS15231B / ESP32-S3 / 320×480
 *   Nuevo:    JC1060P470C_I / MIPI-DSI / JD9165BA / ESP32-P4 / 1024×600
 */

#pragma once

#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"

/* ── Formato de color ─────────────────────────────────────────────────────── */
#define ESP_LCD_COLOR_FORMAT_RGB565   (1)
#define ESP_LCD_COLOR_FORMAT_RGB888   (2)

#define BSP_LCD_COLOR_FORMAT    (ESP_LCD_COLOR_FORMAT_RGB565)
#define BSP_LCD_BIGENDIAN       (0)
#define BSP_LCD_BITS_PER_PIXEL  (16)
#define BSP_LCD_COLOR_SPACE     (LCD_RGB_ELEMENT_ORDER_RGB)

/* ── Resolución ───────────────────────────────────────────────────────────── */
#define BSP_LCD_H_RES   (1024)
#define BSP_LCD_V_RES   (600)

/*
 * Alias de compatibilidad: el código original (main.c, ui.c) referencia
 * EXAMPLE_LCD_QSPI_H/V_RES. Redirigimos a las nuevas macros para que
 * esos ficheros no necesiten modificación.
 */
#define EXAMPLE_LCD_QSPI_H_RES  BSP_LCD_H_RES
#define EXAMPLE_LCD_QSPI_V_RES  BSP_LCD_V_RES

/* ── MIPI-DSI — bus y timings ─────────────────────────────────────────────── */
#define BSP_LCD_MIPI_DSI_LANE_NUM           (2)
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS  (750)

/* Pixel clock: DOTCLK = 51.2 MHz → 52 MHz (redondeo estándar IDF) */
#define BSP_LCD_MIPI_DPI_CLK_MHZ            (52)

/* Horizontal — fuente: dtsi oficial (HS=24, HBP=136, HFP=160) */
#define BSP_LCD_MIPI_HSYNC_PULSE_WIDTH      (24)
#define BSP_LCD_MIPI_HSYNC_BACK_PORCH       (160)
#define BSP_LCD_MIPI_HSYNC_FRONT_PORCH      (160)

/* Vertical — fuente: dtsi oficial (VS=2, VBP=21, VFP=12) */
#define BSP_LCD_MIPI_VSYNC_PULSE_WIDTH      (10)
#define BSP_LCD_MIPI_VSYNC_BACK_PORCH       (21)
#define BSP_LCD_MIPI_VSYNC_FRONT_PORCH      (12)

/* ── LDO interno ESP32-P4 para alimentar el PHY MIPI-DSI ─────────────────── */
#define BSP_MIPI_DSI_PHY_PWR_LDO_CHAN         (3)      /* LDO_VO3 → VDD_MIPI_DPHY */
#define BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  (2500)

/* ── GPIO — ajustar según el esquemático de tu módulo ────────────────────── */

/* Retroiluminación: control PWM mediante LEDC */
#define BSP_LCD_BACKLIGHT   (GPIO_NUM_23)

/* Reset del panel (activo bajo, 0 ms mínimo según JD9165BA datasheet).
 *
 * El esquematico real conecta LCD_RST a GPIO 0, pero GPIO 0 es strapping
 * pin del ESP32-P4 (selecciona modo boot UART/JTAG vs SPI Flash). Si por
 * cualquier razón se pulsa GPIO 0 a LOW durante un reset del SoC, el
 * bootloader podria entrar en modo download.
 *
 * Solucion segura: dejar el reset NO conectado (GPIO_NUM_NC). El JD9165BA
 * arranca correctamente con su power-on reset interno — verificado en
 * produccion durante meses (cuando estaba mal asignado a GPIO 27, que es
 * en realidad RX1 del MAX485 onboard, la pantalla funcionaba igual).
 *
 * Si en el futuro se necesita reset por software, usar GPIO 0 con cuidado
 * o mover a un GPIO libre que no sea strapping pin. */
#define BSP_LCD_RST         (GPIO_NUM_NC)

/* I2C — Touch GT911 */
#define BSP_I2C_SDA         (GPIO_NUM_7)
#define BSP_I2C_SCL         (GPIO_NUM_8)
#define BSP_I2C_NUM         (I2C_NUM_1)
#define BSP_I2C_CLK_HZ      (400000)

/* GT911 interrupt y reset (usar GPIO_NUM_NC si no están cableados al ESP) */
#define BSP_LCD_TOUCH_INT   (GPIO_NUM_47)
#define BSP_LCD_TOUCH_RST   (GPIO_NUM_48)

/* ── Buffer de renderizado LVGL ──────────────────────────────────────────── */
/* 50 líneas × 1024 px = 51 200 píxeles; reside en PSRAM del ESP32-P4      */
#define BSP_LCD_DRAW_BUFF_SIZE    (BSP_LCD_H_RES * 50)
#define BSP_LCD_DRAW_BUFF_DOUBLE  (0)

/* Número de frame-buffers DPI del driver MIPI:
   1 → simple (por defecto)
   2 → doble buffer + avoid_tearing (requiere ~2.4 MB PSRAM adicional)     */
#define BSP_LCD_DPI_BUFFER_NUMS   (1)

/* ── LEDC backlight ──────────────────────────────────────────────────────── */
#define BSP_LEDC_TIMER_NUM    (1)
#define BSP_LEDC_CHANNEL_NUM  (1)

/* ── Compatibilidad con la macro de tear del proyecto original ───────────── */
/* El proyecto QSPI usaba BSP_SYNC_TASK_CONFIG. En MIPI-DSI el tearing se
   gestiona a nivel de driver DPI, por lo que esta macro ya no se usa.
   Se define vacía para evitar errores de compilación si algún fichero la
   referencia. */
#define BSP_SYNC_TASK_CONFIG(te_io, intr_type)  {0}

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuración del display BSP (mantenida por compatibilidad). */
typedef struct {
    int dummy;
} bsp_display_config_t;

/** @brief Handles de bajo nivel devueltos por bsp_display_new_with_handles(). */
typedef struct {
    esp_lcd_dsi_bus_handle_t   mipi_dsi_bus;
    esp_lcd_panel_io_handle_t  io;
    esp_lcd_panel_handle_t     panel;
} bsp_lcd_handles_t;

/* Prototipos implementados en esp_bsp.c */
esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t     *ret_panel,
                          esp_lcd_panel_io_handle_t  *ret_io);

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config,
                                       bsp_lcd_handles_t          *ret_handles);

void      bsp_display_delete(void);

esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
