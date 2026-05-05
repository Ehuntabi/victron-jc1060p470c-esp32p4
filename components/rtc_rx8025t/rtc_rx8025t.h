#pragma once
#include <time.h>
#include "esp_err.h"

/**
 * @brief Inicializar el RTC RX8025T en I2C_NUM_0
 *        SCL = GPIO10, SDA = GPIO12, addr = 0x32
 *
 * Si el RTC no tiene batería y pierde la hora, arranca en
 * 2000-01-01 00:00:00. Usa rtc_set_time() para ajustarla.
 */
esp_err_t rtc_init(void);

/**
 * @brief Leer hora actual del RTC
 * @param tm  Estructura tm rellena (year desde 1900, month 0-11)
 */
esp_err_t rtc_get_time(struct tm *tm_out);

/**
 * @brief Escribir hora en el RTC
 * @param tm  Estructura tm con la hora a guardar
 */
esp_err_t rtc_set_time(const struct tm *tm_in);

/**
 * @brief Obtener timestamp Unix desde el RTC
 */
time_t rtc_get_timestamp(void);
