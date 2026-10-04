/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_cam_sensor_types.h"

#include "driver/i2c_master.h"
#include "esp_err.h"

#define OV02C10_SCCB_ADDR   0x36

/* Reset por software del sensor + reescritura de su tabla de modo, por I2C y sin
 * tocar el CSI/ISP del P4 (ver el comentario en ov02c10.c). */
esp_err_t ov02c10_recover_over_i2c(i2c_master_bus_handle_t bus);

/**
 * @brief Power on camera sensor device and detect the device connected to the designated sccb bus.
 *
 * @param[in] config Configuration related to device power-on and detection.
 * @return
 *      - Camera device handle on success, otherwise, failed.
 */
esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif
