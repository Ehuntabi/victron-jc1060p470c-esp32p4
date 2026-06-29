/* Servicio de camara MIPI-CSI (SC2336). Hito 1: bring-up + medir luz.
 * Encapsula esp_video/esp_cam_sensor. El resto del firmware no conoce el pipeline CSI.
 */
#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa la camara SC2336 por MIPI-CSI reutilizando el bus I2C dado
 * (el del proyecto, GPIO 7/8, compartido con touch/RTC). Auto-detecta el sensor.
 * Devuelve ESP_OK si esp_video_init() y la deteccion del sensor van bien.
 * Llamar DESPUES de bsp_i2c_init() (el handle no puede ser NULL).
 */
esp_err_t camera_init(i2c_master_bus_handle_t i2c);

#ifdef __cplusplus
}
#endif
