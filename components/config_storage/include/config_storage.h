// config_storage.h
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "nvs.h"

#define DEFAULT_AP_PASSWORD "12345678"

// brightness settings
esp_err_t load_brightness(uint8_t *brightness_out);
esp_err_t save_brightness(uint8_t brightness);

// AES key handling
// Legacy single AES key handling (for compatibility)
esp_err_t load_aes_key(uint8_t key_out[16]);
esp_err_t save_aes_key(const uint8_t key_in[16]);

// New multiple Victron device configuration
#define VICTRON_MAX_DEVICES 8

typedef struct {
    char mac_address[18];    // "XX:XX:XX:XX:XX:XX" format
    uint8_t aes_key[16];     // 16-byte AES key
    char device_name[32];    // User-friendly device name
    bool enabled;            // Whether this device is active
} victron_device_config_t;

esp_err_t load_victron_devices(victron_device_config_t *devices_out, 
                               uint8_t *count_out, 
                               uint8_t max_devices);
esp_err_t save_victron_devices(const victron_device_config_t *devices, 
                               uint8_t count);

// Helper function to add a single device
esp_err_t add_victron_device(const uint8_t mac[6], const uint8_t aes_key[16]);

// Screensaver settings
esp_err_t load_screensaver_settings(bool *enabled, uint8_t *brightness, uint16_t *timeout);
esp_err_t save_screensaver_settings(bool enabled, uint8_t brightness, uint16_t timeout);
esp_err_t load_screensaver_mode(uint8_t *mode_out, uint8_t *rotate_period_min_out);
esp_err_t save_screensaver_mode(uint8_t mode, uint8_t rotate_period_min);

// Wi‑Fi AP settings handling (NVS namespace: "wifi")
// ssid_out and pass_out must have space for ssid_len and pass_len, respectively.
// On return, *ssid_len and *pass_len are set to the actual string lengths (including null).
esp_err_t load_wifi_config(char *ssid_out, size_t *ssid_len,
                           char *pass_out, size_t *pass_len,
                           uint8_t *enabled_out);

// Save AP settings; ssid and pass should be null‑terminated strings.
esp_err_t save_wifi_config(const char *ssid,
                           const char *pass,
                           uint8_t enabled_out);

// Relay tab configuration persistence
esp_err_t load_relay_config(bool *enabled_out,
                            uint8_t *count_out,
                            uint8_t *pins_out,
                            char (*labels_out)[20],
                            size_t max_pins);

esp_err_t save_relay_config(bool enabled,
                            const uint8_t *pins,
                            const char (*labels)[20],
                            uint8_t count);

// Victron BLE debug flag persistence (NVS namespace: "debug")
esp_err_t load_victron_debug(bool *enabled_out);
esp_err_t save_victron_debug(bool enabled);

// UI view mode selection (NVS namespace: "display")
esp_err_t load_ui_view_mode(uint8_t *mode_out);
esp_err_t save_ui_view_mode(uint8_t mode);

// Night mode (auto brightness by RTC hour). NVS namespace: "display"
// start_h / end_h en formato 0..23. Si start==end, la ventana es vacía.
// Si start>end, la ventana cruza la medianoche (ej. 22 → 7).
esp_err_t load_night_mode(bool *enabled_out,
                          uint8_t *start_h_out,
                          uint8_t *end_h_out,
                          uint8_t *brightness_out);
esp_err_t save_night_mode(bool enabled,
                          uint8_t start_h,
                          uint8_t end_h,
                          uint8_t brightness);
