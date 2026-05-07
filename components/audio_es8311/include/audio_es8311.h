#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(i2c_master_bus_handle_t bus);
esp_err_t audio_beep(int freq_hz, int duration_ms);

#ifdef __cplusplus
}
#endif
