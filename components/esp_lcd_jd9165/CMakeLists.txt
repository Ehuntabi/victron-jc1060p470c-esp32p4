# Fork local. Original: espressif/esp_lcd_jd9165 1.0.4 (esp-iot-solution).
# Diferencia: panel_jd9165_init() no llama a esp_lcd_panel_io_rx_param para
# leer el Display ID (causaba cuelgue 5 s + WDT en algunos paneles JD9165).
idf_component_register(SRCS "esp_lcd_jd9165.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_lcd")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_JD9165_VER_MAJOR=1
    ESP_LCD_JD9165_VER_MINOR=0
    ESP_LCD_JD9165_VER_PATCH=4)
