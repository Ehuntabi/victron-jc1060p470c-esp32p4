#pragma once
#include "esp_err.h"
#include "frigo.h"

/**
 * @brief Inicializar SD (slot 1, GPIO matrix, 1-bit) y montar FAT en /sdcard
 *        CLK=GPIO36, CMD=GPIO35, D0=GPIO37
 */
esp_err_t datalogger_init(void);

/**
 * @brief Registrar una muestra en el CSV del día actual.
 *        Llama a rtc_get_time() si el RTC está disponible,
 *        si no usa millis como timestamp provisional.
 *
 * @param frigo  Estado actual del frigo (temperaturas + ventilador)
 */
esp_err_t datalogger_log(const frigo_state_t *frigo);

/**
 * @brief Devuelve true si la SD está montada y operativa
 */
bool datalogger_is_ready(void);
