idf_component_register(SRCS "puente.c" "simulador.c" "satelite.c"
    PRIV_REQUIRES nvs_flash bt esp_wifi esp_netif esp_event esp_http_client
                   console mbedtls esp_driver_uart)
