/*
 * Driver local OV02C10 para esp_cam_sensor (board Guition JC1060P470C).
 * El sensor de esta board es un OmniVision OV02C10 (NO un SC2336): SCCB 0x36,
 * chip ID 0x5602, RAW10 1928x1092, MIPI-CSI 2 lanes. esp_cam_sensor no trae
 * driver OV02C10, asi que se porta aqui (componente local) desde el driver
 * mainline de Linux usando sc2336.c como plantilla de estructura.
 */
#pragma once

#include "esp_cam_sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OV02C10_SCCB_ADDR   0x36
#define OV02C10_PID         0x5602
#define OV02C10_SENSOR_NAME "OV02C10"

/**
 * @brief Enciende y detecta el OV02C10 en el bus SCCB indicado.
 *
 * @param[in] config Configuracion de power-on y deteccion.
 * @return handle del sensor en exito, NULL si falla.
 */
esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif
