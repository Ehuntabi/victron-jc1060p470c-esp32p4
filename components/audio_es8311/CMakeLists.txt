idf_component_register(
    SRCS "audio_es8311.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_i2c log freertos nvs_flash
    REQUIRES esp_codec_dev
)
