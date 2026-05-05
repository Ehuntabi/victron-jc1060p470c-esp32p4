idf_component_register(
    SRCS "victron_ble.c" "victron_products.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_netif nvs_flash bt esp_hw_support mbedtls config_storage
)

set(NIMBLE_BASE $ENV{IDF_PATH}/components/bt/host/nimble)
set(BT_BASE $ENV{IDF_PATH}/components/bt)

target_include_directories(__idf_victron_ble PRIVATE
    ${NIMBLE_BASE}/nimble/porting/nimble/include
    ${NIMBLE_BASE}/nimble/porting/npl/freertos/include
    ${NIMBLE_BASE}/nimble/nimble/include
    ${NIMBLE_BASE}/nimble/nimble/host/include
    ${NIMBLE_BASE}/nimble/nimble/host/services/gap/include
    ${NIMBLE_BASE}/nimble/nimble/host/services/gatt/include
    ${NIMBLE_BASE}/nimble/nimble/host/store/config/include
    ${NIMBLE_BASE}/nimble/nimble/host/util/include
    ${NIMBLE_BASE}/nimble/nimble/transport/include
    ${NIMBLE_BASE}/port/include
    ${BT_BASE}/porting/npl/freertos/include
    ${BT_BASE}/porting/transport/include
)

# Enlazar con la librería NimBLE del componente bt
target_link_libraries(__idf_victron_ble PRIVATE
    idf::bt
)
