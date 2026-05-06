#pragma once
#include <time.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief Inicializar el RTC RX8130 usando el bus I2C existente del BSP
 *        El RX8130 comparte I2C_NUM_1 (GPIO7=SDA, GPIO8=SCL) con el touch GT911
 *
 * @param bus  Handle del bus I2C obtenido con bsp_i2c_get_handle()
 * @return ESP_OK si el chip responde, ESP_ERR_NOT_FOUND si no hay respuesta
 */
esp_err_t rtc_init(i2c_master_bus_handle_t bus);

/**
 * @brief Devuelve true si el RTC se inicializó correctamente
 */
bool rtc_is_ready(void);

/**
 * @brief Leer hora actual del RTC
 */
esp_err_t rtc_get_time(struct tm *tm_out);

/**
 * @brief Escribir hora en el RTC
 */
esp_err_t rtc_set_time(const struct tm *tm_in);

/**
 * @brief Obtener timestamp Unix desde el RTC
 */
time_t rtc_get_timestamp(void);
