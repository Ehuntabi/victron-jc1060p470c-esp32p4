#include "victron_ble.h"
#include "config_storage.h"
#include "victron_records.h"
#include "victron_products.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "aes/esp_aes.h"

static const char *TAG = "victron_ble";

static bool victron_debug_enabled = false;
#define VDBG(fmt, ...) do { if (victron_debug_enabled) ESP_LOGI(TAG, fmt, ##__VA_ARGS__); } while(0)

#define NA_U16_SIGNED   0x7FFF
#define NA_U16_UNSIGNED 0xFFFF
#define NA_U8           0xFF
#define NA_U9           0x1FF
#define NA_U10          0x3FF
#define NA_U22          0x3FFFFF

// Multiple device configuration
static victron_device_config_t device_configs[VICTRON_MAX_DEVICES];
static uint8_t device_count = 0;
static uint8_t legacy_aes_key[16];  // Fallback for single-key compatibility

typedef enum {
    VICTRON_MANUFACTURER_RECORD_PRODUCT_ADVERTISEMENT = 0x10,
} victron_manufacturer_record_type_t;

typedef struct __attribute__((packed)) {
    uint16_t vendorID;
    uint8_t  manufacturer_record_type;
    uint8_t  manufacturer_record_length;
    uint16_t product_id;
    uint8_t  victronRecordType;
    uint16_t nonceDataCounter;
    uint8_t  encryptKeyMatch;
    uint8_t  victronEncryptedData[VICTRON_ENCRYPTED_DATA_MAX_SIZE];
    uint8_t  nullPad;
} victronManufacturerData;

static victron_data_cb_t data_cb = NULL;
void victron_ble_register_callback(victron_data_cb_t cb) { data_cb = cb; }

static void ble_host_task(void *param);
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static void ble_app_on_sync(void);

static inline int32_t sign_extend(uint32_t value, uint8_t bits)
{
    uint32_t shift = 32u - bits;
    return (int32_t)(value << shift) >> shift;
}

/* -------------------------------------------------------------------------- */
/*  Device Configuration Lookup                                               */
/* -------------------------------------------------------------------------- */

static const victron_device_config_t* find_device_config_by_mac(const uint8_t mac[6])
{
    // Format the MAC address as string for comparison (BLE addresses are stored in reverse order)
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

    // Search for matching device configuration
    for (int i = 0; i < device_count; i++) {
        if (device_configs[i].enabled && 
            strcasecmp(device_configs[i].mac_address, mac_str) == 0) {
            VDBG("Found device config for MAC %s: '%s'", 
                 mac_str, device_configs[i].device_name);
            return &device_configs[i];
        }
    }

    VDBG("No device config found for MAC %s", mac_str);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Initialization                                                            */
/* -------------------------------------------------------------------------- */

void victron_ble_init(void)
{
    ESP_LOGI(TAG, "Initializing NVS for Victron BLE");

    // Keep any pre-registered data_cb
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load multiple device configurations
    if (load_victron_devices(device_configs, &device_count, VICTRON_MAX_DEVICES) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %d Victron device configurations", device_count);
        for (int i = 0; i < device_count; i++) {
            if (device_configs[i].enabled) {
                ESP_LOGI(TAG, "Device %d: MAC=%s, Name='%s', Enabled=%s",
                         i, device_configs[i].mac_address, device_configs[i].device_name,
                         device_configs[i].enabled ? "YES" : "NO");
            }
        }
    } else {
        ESP_LOGW(TAG, "Failed to load device configurations");
        device_count = 0;
    }

    // Load legacy single AES key as fallback for compatibility
    if (load_aes_key(legacy_aes_key) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded legacy AES key for compatibility");
    } else {
        ESP_LOGW(TAG, "No legacy AES key found, using default");
        const uint8_t default_key[16] = {
            0x4B,0x71,0x78,0xE6, 0x4C,0x82,0x8A,0x26,
            0x2C,0xDD,0x51,0x61, 0xE3,0x40,0x4B,0x7A
        };
        memcpy(legacy_aes_key, default_key, sizeof(legacy_aes_key));
    }

    ESP_LOGI(TAG, "Initializing NimBLE stack");
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

void victron_ble_set_debug(bool enabled)
{
    victron_debug_enabled = enabled;
    ESP_LOGI(TAG, "Victron BLE debug set to %s", enabled ? "ENABLED" : "disabled");
}

void victron_ble_reload_device_config(void)
{
    ESP_LOGI(TAG, "Reloading Victron device configuration");
    
    if (load_victron_devices(device_configs, &device_count, VICTRON_MAX_DEVICES) == ESP_OK) {
        ESP_LOGI(TAG, "Reloaded %d Victron device configurations", device_count);
        for (int i = 0; i < device_count; i++) {
            if (device_configs[i].enabled) {
                ESP_LOGI(TAG, "Device %d: MAC=%s, Name='%s', Enabled=%s",
                         i, device_configs[i].mac_address, device_configs[i].device_name,
                         device_configs[i].enabled ? "YES" : "NO");
            }
        }
        
        // Refresh the UI device list to show the updated configuration
        extern void ui_refresh_victron_device_list(void);
        ui_refresh_victron_device_list();
    } else {
        ESP_LOGW(TAG, "Failed to reload device configurations");
        device_count = 0;
    }
}

/* -------------------------------------------------------------------------- */
/*  BLE Stack                                                                 */
/* -------------------------------------------------------------------------- */

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_app_on_sync(void)
{
    struct ble_gap_disc_params disc_params = {
        .itvl = 0x0060, .window = 0x0030,
        .passive = 1, .filter_policy = 0, .limited = 0
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &disc_params, ble_gap_event_handler, NULL);
    if (rc) {
        ESP_LOGE(TAG, "Error starting discovery; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Started Victron BLE scan");
    }
}

/* -------------------------------------------------------------------------- */
/*  Utility                                                                   */
/* -------------------------------------------------------------------------- */

static const char* get_device_type_name(uint8_t type)
{
    switch (type) {
        case 0x00: return "Test Record";
        case 0x01: return "Solar Charger";
        case 0x02: return "Battery Monitor";
        case 0x03: return "Inverter";
        case 0x04: return "DC/DC Converter";
        case 0x05: return "SmartLithium";
        case 0x06: return "Inverter RS";
        case 0x07: return "GX-Device";
        case 0x08: return "AC Charger";
        case 0x09: return "Smart Battery Protect";
        case 0x0A: return "Lynx Smart BMS";
        case 0x0B: return "Multi RS";
        case 0x0C: return "VE.Bus";
        case 0x0D: return "DC Energy Meter";
        default:   return "Unknown/Reserved";
    }
}

/* -------------------------------------------------------------------------- */
/*  GAP Event Handler                                                         */
/* -------------------------------------------------------------------------- */

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC)
        return 0;

    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
    if (rc != 0) {
        ESP_LOGV(TAG, "Failed to parse advertisement fields, rc=%d", rc);
        return 0;
    }

    if (fields.mfg_data_len < offsetof(victronManufacturerData, victronEncryptedData) + 1)
        return 0;

    victronManufacturerData *mdata = (void*)fields.mfg_data;
    if (mdata->vendorID != VICTRON_MANUFACTURER_ID)
        return 0;

    if (mdata->manufacturer_record_type != VICTRON_MANUFACTURER_RECORD_PRODUCT_ADVERTISEMENT) {
        VDBG("Skipping manufacturer record type 0x%02X",
             (unsigned)mdata->manufacturer_record_type);
        return 0;
    }

    uint16_t product_id = mdata->product_id;
    const char *product_name = victron_product_name(product_id);
    if (victron_debug_enabled) {
        if (product_name) {
            ESP_LOGI(TAG, "Product ID: 0x%04X (%s)", product_id, product_name);
        } else {
            ESP_LOGI(TAG, "Product ID: 0x%04X (unknown)", product_id);
        }
    }

    // Look up device configuration by MAC address
    const victron_device_config_t* device_config = find_device_config_by_mac(event->disc.addr.val);
    const uint8_t* device_aes_key = NULL;
    
    if (device_config != NULL) {
        device_aes_key = device_config->aes_key;
        VDBG("Using device-specific AES key for %s (MAC: %s)", 
             device_config->device_name, device_config->mac_address);
    } else {
        // Fallback to legacy single key for backward compatibility
        device_aes_key = legacy_aes_key;
        if (device_count > 0) {
            VDBG("Device not found in %d configured devices, using legacy key", device_count);
        } else {
            VDBG("No devices configured, using legacy AES key");
        }
    }

    // Verbose packet log
    VDBG("=== Victron BLE Packet Received ===");
    VDBG("MAC: %02X:%02X:%02X:%02X:%02X:%02X",
         event->disc.addr.val[5], event->disc.addr.val[4],
         event->disc.addr.val[3], event->disc.addr.val[2],
         event->disc.addr.val[1], event->disc.addr.val[0]);
    VDBG("Vendor ID: 0x%04X, Record: 0x%02X (%s)",
         mdata->vendorID, mdata->victronRecordType,
         get_device_type_name(mdata->victronRecordType));
    VDBG("Nonce: 0x%04X, KeyMatch: 0x%02X (device_key[0]=0x%02X)",
         mdata->nonceDataCounter, mdata->encryptKeyMatch, device_aes_key[0]);
    if (victron_debug_enabled)
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, fields.mfg_data, fields.mfg_data_len, ESP_LOG_INFO);

    // Check if we should ignore unknown devices
    if (device_config == NULL && device_count > 0) {
        // If we have configured devices, ignore packets from unknown MACs
        ESP_LOGD(TAG, "Ignoring packet from unknown MAC address (not in configured device list)");
        return 0;
    }

    if (mdata->encryptKeyMatch != device_aes_key[0]) {
        ESP_LOGW(TAG, "Key mismatch! Device key[0]=0x%02X, ours=0x%02X - skipping decrypt",
                 mdata->encryptKeyMatch, device_aes_key[0]);
        return 0;
    }

    int encr_size = fields.mfg_data_len - offsetof(victronManufacturerData, victronEncryptedData);
    if (encr_size <= 0 || encr_size > 25) {
        ESP_LOGW(TAG, "Invalid encrypted data size: %d", encr_size);
        return 0;
    }

    uint8_t input[VICTRON_ENCRYPTED_DATA_MAX_SIZE] = {0};
    uint8_t output[VICTRON_ENCRYPTED_DATA_MAX_SIZE] = {0};
    memcpy(input, mdata->victronEncryptedData, encr_size);

    if (victron_debug_enabled) {
        ESP_LOGI(TAG, "Encrypted payload:");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, input, encr_size, ESP_LOG_INFO);
    }

    /* ---------------- AES CTR Decrypt ---------------- */
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, device_aes_key, 128)) {
        ESP_LOGE(TAG, "AES setkey failed");
        esp_aes_free(&ctx);
        return 0;
    }

    // Only first 2 bytes used for Victron nonce; rest = zero
    uint16_t nonce = mdata->nonceDataCounter;
    uint8_t ctr_blk[16] = { (uint8_t)(nonce & 0xFF), (uint8_t)(nonce >> 8) };
    uint8_t stream_block[16] = {0};
    size_t offset = 0;

    rc = esp_aes_crypt_ctr(&ctx, encr_size, &offset, ctr_blk, stream_block, input, output);
    esp_aes_free(&ctx);
    if (rc) {
        ESP_LOGE(TAG, "AES CTR decrypt failed, rc=%d", rc);
        return 0;
    }

    if (victron_debug_enabled) {
        ESP_LOGI(TAG, "Decrypted payload (nonce=0x%04X):", nonce);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, output, encr_size, ESP_LOG_INFO);
    }

    ui_set_ble_mac(event->disc.addr.val);
    const victron_record_type_t rec_type = (victron_record_type_t)mdata->victronRecordType;

    /* ---------------- Record Parsing ---------------- */
    switch (rec_type) {
        case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
            const victron_record_solar_charger_t *r = (const victron_record_solar_charger_t *)output;

            uint16_t load_raw = (uint16_t)output[10] | ((uint16_t)(output[11] & 0x01) << 8);
            float load_current_A = (load_raw == 0x1FF) ? 0.0f : load_raw / 10.0f;

            ESP_LOGI(TAG, "=== SmartSolar Charger ===");
            ESP_LOGI(TAG, "Vbat=%.2fV Ibat=%.1fA PV=%uW Yield=%.2fkWh Load=%.1fA",
                     r->battery_voltage_centi / 100.0f,
                     r->battery_current_deci / 10.0f,
                     r->pv_power_w,
                     r->yield_today_centikwh / 100.0f,
                     load_current_A);

            if (data_cb) {
                victron_data_t parsed = {
                    .type = VICTRON_BLE_RECORD_SOLAR_CHARGER,
                    .product_id = product_id
                };
                parsed.record.solar.device_state = r->device_state;
                parsed.record.solar.charger_error = r->charger_error;
                parsed.record.solar.battery_voltage_centi = r->battery_voltage_centi;
                parsed.record.solar.battery_current_deci = r->battery_current_deci;
                parsed.record.solar.yield_today_centikwh = r->yield_today_centikwh;
                parsed.record.solar.pv_power_w = r->pv_power_w;
                parsed.record.solar.load_current_deci = load_raw;
                data_cb(&parsed);
            }
            break;
        }

        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            const uint8_t *b = output;

            uint16_t ttg_raw     = b[0] | (b[1] << 8);
            uint16_t voltage_raw = b[2] | (b[3] << 8);
            uint16_t alarm_raw   = b[4] | (b[5] << 8);
            uint16_t aux_raw     = b[6] | (b[7] << 8);

            uint64_t tail = 0;
            for (int i = 0; i < 7; i++)
                tail |= ((uint64_t)b[8 + i]) << (8 * i);

            uint8_t aux_input = tail & 0x03; tail >>= 2;
            int32_t current_bits  = sign_extend(tail & ((1u << 22) - 1u), 22); tail >>= 22;
            int32_t consumed_bits = sign_extend(tail & ((1u << 20) - 1u), 20); tail >>= 20;
            uint32_t soc_bits = tail & ((1u << 10) - 1u);

            float current_A   = current_bits / 1000.0f;

            ESP_LOGI(TAG, "=== Battery Monitor ===");
            ESP_LOGI(TAG, "Vbat=%.2fV Ibat=%.3fA SOC=%.1f%% TTG=%u min",
                     voltage_raw / 100.0f, current_A, soc_bits / 10.0f, ttg_raw);
            ESP_LOGI(TAG, "Aux=%u (%.2fV), Alarm=0x%04X",
                     aux_input, aux_raw / 100.0f, alarm_raw);

            if (data_cb) {
                victron_data_t parsed = {
                    .type = VICTRON_BLE_RECORD_BATTERY_MONITOR,
                    .product_id = product_id
                };
                parsed.record.battery.time_to_go_minutes = ttg_raw;
                parsed.record.battery.battery_voltage_centi = voltage_raw;
                parsed.record.battery.alarm_reason = alarm_raw;
                parsed.record.battery.aux_value = aux_raw;
                parsed.record.battery.aux_input = aux_input;
                parsed.record.battery.battery_current_milli = current_bits;
                parsed.record.battery.consumed_ah_deci = consumed_bits;
                parsed.record.battery.soc_deci_percent = soc_bits;
                data_cb(&parsed);
            }
            break;
        }

        case VICTRON_BLE_RECORD_INVERTER: {
            if (encr_size < 11) {
                ESP_LOGW(TAG, "Inverter payload too short: %d", encr_size);
                break;
            }

            const uint8_t *b = output;
            uint8_t device_state = b[0];
            uint16_t alarm_reason = b[1] | (b[2] << 8);
            int16_t battery_voltage_centi = (int16_t)(b[3] | (b[4] << 8));
            uint16_t ac_apparent_power_va = b[5] | (b[6] << 8);

            uint32_t tail = (uint32_t)b[7]
                          | ((uint32_t)b[8] << 8)
                          | ((uint32_t)b[9] << 16)
                          | ((uint32_t)b[10] << 24);
            uint16_t ac_voltage_centi = (uint16_t)(tail & 0x7FFFu);
            uint16_t ac_current_deci = (uint16_t)((tail >> 15) & 0x7FFu);

            ESP_LOGI(TAG, "=== Inverter ===");
            ESP_LOGI(TAG, "Vbat=%.2fV AC=%.2fV Iac=%.1fA P=%uVA",
                     battery_voltage_centi / 100.0f,
                     ac_voltage_centi / 100.0f,
                     ac_current_deci / 10.0f,
                     (unsigned)ac_apparent_power_va);

            if (data_cb) {
                victron_data_t parsed = {
                    .type = VICTRON_BLE_RECORD_INVERTER,
                    .product_id = product_id
                };
                parsed.record.inverter.device_state = device_state;
                parsed.record.inverter.alarm_reason = alarm_reason;
                parsed.record.inverter.battery_voltage_centi = battery_voltage_centi;
                parsed.record.inverter.ac_apparent_power_va = ac_apparent_power_va;
                parsed.record.inverter.ac_voltage_centi = ac_voltage_centi;
                parsed.record.inverter.ac_current_deci = ac_current_deci;
                data_cb(&parsed);
            }
            break;
        }

        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            if (encr_size < 10) {
                ESP_LOGW(TAG, "DC/DC payload too short: %d", encr_size);
                break;
            }

            const uint8_t *b = output;
            uint8_t device_state = b[0];
            uint8_t charger_error = b[1];
            uint16_t input_voltage_centi = (uint16_t)(b[2] | (b[3] << 8));
            uint16_t output_voltage_centi = (uint16_t)(b[4] | (b[5] << 8));
            uint32_t off_reason = (uint32_t)b[6]
                                | ((uint32_t)b[7] << 8)
                                | ((uint32_t)b[8] << 16)
                                | ((uint32_t)b[9] << 24);

            ESP_LOGI(TAG, "=== DC/DC Converter ===");
            ESP_LOGI(TAG, "State=%u Error=0x%02X Vin=%.2fV Vout=%.2fV OffReason=0x%08lX",
                     (unsigned)device_state,
                     (unsigned)charger_error,
                     input_voltage_centi / 100.0f,
                     output_voltage_centi / 100.0f,
                     (unsigned long)off_reason);

            if (data_cb) {
                victron_data_t parsed = {
                    .type = VICTRON_BLE_RECORD_DCDC_CONVERTER,
                    .product_id = product_id
                };
                parsed.record.dcdc.device_state = device_state;
                parsed.record.dcdc.charger_error = charger_error;
                parsed.record.dcdc.input_voltage_centi = input_voltage_centi;
                parsed.record.dcdc.output_voltage_centi = output_voltage_centi;
                parsed.record.dcdc.off_reason = off_reason;
                data_cb(&parsed);
            }
            break;
        }

        case VICTRON_BLE_RECORD_SMART_LITHIUM: {
            if (encr_size < 16) {
                ESP_LOGW(TAG, "Smart Lithium payload too short: %d", encr_size);
                break;
            }

            const uint8_t *b = output;
            uint32_t bms_flags = (uint32_t)b[0]
                               | ((uint32_t)b[1] << 8)
                               | ((uint32_t)b[2] << 16)
                               | ((uint32_t)b[3] << 24);
            uint16_t error_flags = (uint16_t)(b[4] | (b[5] << 8));
            uint8_t cell_values[8];
            for (int i = 0; i < 8; ++i) {
                cell_values[i] = b[6 + i];
            }
            uint16_t packed_voltage = (uint16_t)(b[14] | (b[15] << 8));
            uint16_t battery_voltage_centi = (packed_voltage & 0x0FFFu);
            uint8_t balancer_status = (uint8_t)((packed_voltage >> 12) & 0x0Fu);
            uint8_t temperature_raw = (encr_size > 16) ? b[16] : 0;
            int temperature_c = (int)temperature_raw - 40;

            ESP_LOGI(TAG, "=== Smart Lithium ===");
            ESP_LOGI(TAG, "Flags=0x%08lX Err=0x%04X Batt=%.2fV Temp=%dC",
                     (unsigned long)bms_flags,
                     (unsigned)error_flags,
                     battery_voltage_centi / 100.0f,
                     temperature_c);

            if (data_cb) {
                victron_data_t parsed = {
                    .type = VICTRON_BLE_RECORD_SMART_LITHIUM,
                    .product_id = product_id
                };
                parsed.record.lithium.bms_flags = bms_flags;
                parsed.record.lithium.error_flags = error_flags;
                parsed.record.lithium.cell1_centi = cell_values[0];
                parsed.record.lithium.cell2_centi = cell_values[1];
                parsed.record.lithium.cell3_centi = cell_values[2];
                parsed.record.lithium.cell4_centi = cell_values[3];
                parsed.record.lithium.cell5_centi = cell_values[4];
                parsed.record.lithium.cell6_centi = cell_values[5];
                parsed.record.lithium.cell7_centi = cell_values[6];
                parsed.record.lithium.cell8_centi = cell_values[7];
                parsed.record.lithium.battery_voltage_centi = battery_voltage_centi;
                parsed.record.lithium.balancer_status = balancer_status;
                parsed.record.lithium.temperature_c = temperature_raw;
                data_cb(&parsed);
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unsupported record type 0x%02X (%s)",
                     rec_type, get_device_type_name(rec_type));
            break;
    }

    return 0;
}
