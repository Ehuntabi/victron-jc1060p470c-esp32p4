/*
 * esp_bsp.c — BSP para Guition JC1060P470C_I
 *             ESP32-P4 · MIPI-DSI 2 lanes · JD9165BA · GT911
 *
 * PORT desde victronsolardisplayesp-multi-device
 *   Original: ESP32-S3 / QSPI / AXS15231B / 320×480
 *   Nuevo:    ESP32-P4 / MIPI-DSI / JD9165BA / 1024×600
 *
 * Secuencia de init del panel: tomada literalmente del fichero oficial
 *   MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_2lane.dtsi
 *
 * Todo lo demás (ui.c, victron_ble, config_server…) no cambia.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

/* Controlador JD9165BA */
#include "esp_lcd_jd9165.h"

/* Touch GT911 */
#include "esp_lcd_touch_gt911.h"

#include "esp_bsp.h"
#include "display.h"


/* Init sequence exacta del Demo Guition JC1060P470C_I */
static const jd9165_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49,0x61,0x02,0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A,0x1A,0x0B,0x0D,0x0D,0x11,0x10,0x06,0x08,0x1F,0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16,0x1B,0x0B,0x0D,0x0D,0x11,0x10,0x07,0x09,0x1E,0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16,0x1B,0x0D,0x0B,0x0D,0x11,0x10,0x1C,0x1E,0x09,0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A,0x1A,0x0D,0x0B,0x0D,0x11,0x10,0x1D,0x1F,0x08,0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36,0x2C,0x2E,0x3C,0x38,0x35,0x35,0x32,0x2E,0x1D,0x2B,0x21,0x16,0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36,0x2C,0x2E,0x3C,0x38,0x35,0x35,0x32,0x2E,0x1D,0x2B,0x21,0x16,0x29}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};


static const char *TAG = "bsp_jc1060";

/* ─────────────────────────────────────────────────────────────────────────────
 * Secuencia de inicialización JD9165BA
 * Fuente: MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_2lane.dtsi
 * (se omiten las entradas REGFLAG_DELAY y REGFLAG_END_OF_TABLE, que el
 *  driver de IDF maneja mediante el campo delay_ms de jd9165_lcd_init_cmd_t)
 * ───────────────────────────────────────────────────────────────────────────*/

/* ── Estado interno ───────────────────────────────────────────────────────── */
static bool                      s_i2c_init      = false;
static i2c_master_bus_handle_t   s_i2c_handle    = NULL;
static bsp_lcd_handles_t         s_lcd_handles   = {0};
static esp_ldo_channel_handle_t  s_dsi_ldo       = NULL;
static esp_lcd_touch_handle_t    s_touch         = NULL;
static esp_lcd_panel_io_handle_t s_tp_io         = NULL;

#if LVGL_VERSION_MAJOR >= 9
static lv_display_t *s_disp      = NULL;
#else
static lv_disp_t    *s_disp      = NULL;
#endif
static lv_indev_t   *s_disp_indev = NULL;

/* ════════════════════════════════════════════════════════════════════════════
 * I2C
 * ════════════════════════════════════════════════════════════════════════════*/

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_init) return ESP_OK;

    const i2c_master_bus_config_t cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .sda_io_num        = BSP_I2C_SDA,
        .scl_io_num        = BSP_I2C_SCL,
        .i2c_port          = BSP_I2C_NUM,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_handle),
                        TAG, "i2c_new_master_bus");
    s_i2c_init = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (!s_i2c_init || !s_i2c_handle) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_i2c_handle), TAG, "i2c_del");
    s_i2c_handle = NULL;
    s_i2c_init   = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) { return s_i2c_handle; }

/* ════════════════════════════════════════════════════════════════════════════
 * Backlight — LEDC PWM
 * ════════════════════════════════════════════════════════════════════════════*/

esp_err_t bsp_display_brightness_init(void)
{
    const ledc_timer_config_t tmr = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = BSP_LEDC_TIMER_NUM,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BSP_LEDC_CHANNEL_NUM,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BSP_LEDC_TIMER_NUM,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tmr),  TAG, "ledc_timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc_channel");
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int pct)
{
    if (pct > 100) pct = 100;
    if (pct <   0) pct =   0;
    ESP_LOGI(TAG, "Backlight %d%%", pct);
    uint32_t duty = (1023u * (uint32_t)pct) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL_NUM, duty),
                        TAG, "ledc_set_duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL_NUM),
                        TAG, "ledc_update_duty");
    return ESP_OK;
}

esp_err_t bsp_display_backlight_on(void)  { return bsp_display_brightness_set(100); }
esp_err_t bsp_display_backlight_off(void) { return bsp_display_brightness_set(0);   }

/* ════════════════════════════════════════════════════════════════════════════
 * LDO interno para el PHY MIPI-DSI (ESP32-P4 específico)
 * ════════════════════════════════════════════════════════════════════════════*/

static esp_err_t bsp_enable_dsi_phy_power(void)
{
#if BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0
    const esp_ldo_channel_config_t ldo = {
        .chan_id    = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &s_dsi_ldo),
                        TAG, "ldo_acquire DSI PHY");
    ESP_LOGI(TAG, "MIPI DSI PHY powered (LDO ch%d @ %d mV)",
             BSP_MIPI_DSI_PHY_PWR_LDO_CHAN, BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV);
    /* Esperar a que el LDO se estabilice antes de inicializar el PHY DSI */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Panel MIPI-DSI + JD9165BA
 * ════════════════════════════════════════════════════════════════════════════*/

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config,
                                       bsp_lcd_handles_t          *ret_handles)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness_init");
    /* Test backlight al 100% inmediatamente */
    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(),    TAG, "dsi_phy_power");

    /* ── 1. Bus MIPI-DSI ─────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Init MIPI-DSI bus (%d lanes @ %u Mbps/lane)",
             BSP_LCD_MIPI_DSI_LANE_NUM, BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS);

    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus),
                        TAG, "esp_lcd_new_dsi_bus");

    /* ── 2. IO DBI (canal de comandos/parámetros) ────────────────────────── */
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &io),
                      err, TAG, "esp_lcd_new_panel_io_dbi");

    /* ── 3. Configuración DPI (timing de vídeo) ──────────────────────────── */
    /*
     * Timings del dtsi oficial JD9165BA:
     *   h_size=1024  v_size=600
     *   hsync_pulse=24  hbp=136  hfp=160
     *   vsync_pulse=2   vbp=21   vfp=12
     *   dotclk=51.2 MHz → 52 MHz
     */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BSP_LCD_MIPI_DPI_CLK_MHZ,
        .virtual_channel    = 0,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = BSP_LCD_DPI_BUFFER_NUMS,
        .video_timing = {
            .h_size            = BSP_LCD_H_RES,
            .v_size            = BSP_LCD_V_RES,
            .hsync_pulse_width = BSP_LCD_MIPI_HSYNC_PULSE_WIDTH,
            .hsync_back_porch  = BSP_LCD_MIPI_HSYNC_BACK_PORCH,
            .hsync_front_porch = BSP_LCD_MIPI_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BSP_LCD_MIPI_VSYNC_PULSE_WIDTH,
            .vsync_back_porch  = BSP_LCD_MIPI_VSYNC_BACK_PORCH,
            .vsync_front_porch = BSP_LCD_MIPI_VSYNC_FRONT_PORCH,
        },
        .flags.use_dma2d = true,
    };

    /* ── 4. Panel JD9165BA ───────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Install JD9165BA panel (%dx%d @ %d MHz)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_MIPI_DPI_CLK_MHZ);

    jd9165_vendor_config_t vendor_cfg = {
        .init_cmds      = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(jd9165_lcd_init_cmd_t),
        .mipi_config = {
            .dsi_bus    = mipi_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t panel_dev_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order  = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_cfg,
    };

    /* Enviar init sequence ANTES del DPI panel (bus en modo LP/comando) */
    

    esp_lcd_panel_handle_t disp_panel = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9165(io, &panel_dev_cfg, &disp_panel),
                      err, TAG, "esp_lcd_new_panel_jd9165");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "Panel reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel),  err, TAG, "Panel init");

    /* ── 5. Devolver handles ─────────────────────────────────────────────── */
    ret_handles->mipi_dsi_bus = mipi_dsi_bus;
    ret_handles->io           = io;
    ret_handles->panel        = disp_panel;
    s_lcd_handles             = *ret_handles;

    ESP_LOGI(TAG, "Display OK");
    return ret;

err:
    if (disp_panel)   esp_lcd_panel_del(disp_panel);
    if (io)           esp_lcd_panel_io_del(io);
    if (mipi_dsi_bus) esp_lcd_del_dsi_bus(mipi_dsi_bus);
    return ret;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t     *ret_panel,
                          esp_lcd_panel_io_handle_t  *ret_io)
{
    bsp_lcd_handles_t h = {0};
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(config, &h), TAG, "");
    *ret_panel = h.panel;
    *ret_io    = h.io;
    return ESP_OK;
}

void bsp_display_delete(void)
{
    if (s_lcd_handles.panel)        { esp_lcd_panel_del(s_lcd_handles.panel);           s_lcd_handles.panel        = NULL; }
    if (s_lcd_handles.io)           { esp_lcd_panel_io_del(s_lcd_handles.io);            s_lcd_handles.io           = NULL; }
    if (s_lcd_handles.mipi_dsi_bus) { esp_lcd_del_dsi_bus(s_lcd_handles.mipi_dsi_bus);  s_lcd_handles.mipi_dsi_bus = NULL; }
    if (s_dsi_ldo)                  { esp_ldo_release_channel(s_dsi_ldo);               s_dsi_ldo                  = NULL; }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Touch — GT911 (I2C)
 * ════════════════════════════════════════════════════════════════════════════*/

static esp_err_t bsp_touch_new(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c_init for touch");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(s_i2c_handle, &tp_io_cfg, &s_tp_io),
        TAG, "touch IO i2c");
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(s_tp_io, &tp_cfg, &s_touch),
        TAG, "gt911 init");
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 * LVGL — pantalla DSI + touch
 * ════════════════════════════════════════════════════════════════════════════*/

#if LVGL_VERSION_MAJOR >= 9
static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
#else
static lv_disp_t    *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
#endif
{
    assert(cfg != NULL);

    bsp_lcd_handles_t lcd = {0};
    if (bsp_display_new_with_handles(NULL, &lcd) != ESP_OK) return NULL;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = lcd.io,
        .panel_handle   = lcd.panel,
        .control_handle = NULL,
        .buffer_size    = BSP_LCD_H_RES * BSP_LCD_V_RES, /* pantalla completa para avoid_tearing */
        .double_buffer  = cfg->double_buffer,
        .hres           = BSP_LCD_H_RES,
        .vres           = BSP_LCD_V_RES,
        .monochrome     = false,
        /*
         * El panel es landscape nativo (1024×600).
         * Sin rotación hardware; LVGL aplicará sw_rotate si se configura.
         */
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma    = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes  = false,   /* BSP_LCD_BIGENDIAN = 0 */
#endif
            .sw_rotate   = cfg->flags.sw_rotate,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = (BSP_LCD_DPI_BUFFER_NUMS > 1),
    };

    return lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
}

#if LVGL_VERSION_MAJOR >= 9
static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
#else
static lv_indev_t *bsp_display_indev_init(lv_disp_t *disp)
#endif
{
    if (bsp_touch_new() != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed — sin input device");
        return NULL;
    }
    const lvgl_port_touch_cfg_t tcfg = { .disp = disp, .handle = s_touch };
    return lvgl_port_add_touch(&tcfg);
}

/* ════════════════════════════════════════════════════════════════════════════
 * API pública
 * ════════════════════════════════════════════════════════════════════════════*/

#if LVGL_VERSION_MAJOR >= 9
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
#else
lv_disp_t    *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
#endif
{
    assert(cfg != NULL);

    if (lvgl_port_init(&cfg->lvgl_port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return NULL;
    }

    s_disp = bsp_display_lcd_init(cfg);
    if (!s_disp) { ESP_LOGE(TAG, "LCD init failed"); return NULL; }

    s_disp_indev = bsp_display_indev_init(s_disp);

    return s_disp;
}

lv_indev_t *bsp_display_get_input_dev(void) { return s_disp_indev; }
bool         bsp_display_lock(uint32_t ms)   { return lvgl_port_lock(ms); }
void         bsp_display_unlock(void)        { lvgl_port_unlock(); }
// force rebuild dom 03 may 2026 10:08:40 CEST
// force rebuild 2
// force rebuild 3
// force 5
// force rebuild display
// force rebuild timings
// force ldo delay
// force lp init
// force lp init2
// force backlight test
// force gpio test
// force gpio0 rst
// force touch reactivate
// force full_refresh
// force full_refresh2
// force full_refresh3
// force direct_mode
// force full_refresh_flags
// force 2buf
// force fullscreen buf
// force 1buf
// force ek79007
// force ek79007 clean
// force no init cmds
// force no disp_on_off
// force in_color_format
// force manual callback
// force disp debug
// force jd9165 guition
// force init cmds
// force i2c1
// force i2c 7 8
// force rotation
// force rotation restore
