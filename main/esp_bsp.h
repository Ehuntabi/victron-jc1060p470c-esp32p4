/*
 * esp_bsp.h — API pública BSP para Guition JC1060P470C_I (ESP32-P4 / MIPI-DSI)
 *
 * Esta cabecera mantiene la misma API que el proyecto original (JC3248W535),
 * de modo que main.c, ui.c y el resto del código no necesitan ningún cambio.
 *
 * Diferencias internas (transparentes al usuario):
 *   - Bus LCD:   QSPI  →  MIPI-DSI
 *   - Panel:     AXS15231B  →  JD9165BA
 *   - Touch:     AXS15231B I2C  →  GT911 I2C
 *   - I2C IDF:   i2c_param_config  →  i2c_new_master_bus  (IDF ≥ 5.3)
 *   - LVGL port: lvgl_port_add_disp  →  lvgl_port_add_disp_dsi
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuración LVGL (idéntica a la del proyecto original) ─────────────── */
typedef struct {
    lvgl_port_cfg_t  lvgl_port_cfg;  /*!< Configuración del port LVGL          */
    uint32_t         buffer_size;    /*!< Tamaño del buffer en píxeles          */
    bool             double_buffer;  /*!< true → doble buffer                   */
    struct {
        unsigned int buff_dma    : 1; /*!< Buffer en memoria DMA                */
        unsigned int buff_spiram : 1; /*!< Buffer en PSRAM                      */
        unsigned int sw_rotate   : 1; /*!< Rotación por software (LVGL)         */
    } flags;
#if LVGL_VERSION_MAJOR >= 9
    lv_disp_rotation_t rotate;       /*!< Rotación LVGL 9+                     */
#else
    lv_disp_rot_t      rotate;       /*!< Rotación LVGL 8                      */
#endif
} bsp_display_cfg_t;

/* ── I2C ─────────────────────────────────────────────────────────────────── */
esp_err_t               bsp_i2c_init(void);
esp_err_t               bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/* ── Display + LVGL ──────────────────────────────────────────────────────── */

/**
 * @brief Inicializar display (MIPI-DSI + JD9165BA), touch (GT911) y LVGL.
 *
 * Misma firma que el original. main.c llama a esta función sin cambios.
 *
 * @param cfg  Configuración (no puede ser NULL).
 * @return     Puntero a lv_disp_t/lv_display_t, o NULL si hay error.
 */
#if LVGL_VERSION_MAJOR >= 9
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
#else
lv_disp_t    *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
#endif

/** @brief Obtener el dispositivo de entrada LVGL (touch GT911). */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Tomar el mutex de LVGL antes de llamar a cualquier lv_...().
 * @param timeout_ms  0 bloquea indefinidamente.
 * @return true si se tomó el mutex.
 */
bool bsp_display_lock(uint32_t timeout_ms);

/** @brief Liberar el mutex de LVGL. */
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
