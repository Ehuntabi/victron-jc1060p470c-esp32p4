idf_component_register(
    SRCS "ov02c10.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_cam_sensor
)

# Forzar que el linker conserve el objeto (y con el la seccion .esp_cam_sensor_detect_fn
# del registro a 0x36). Sin esto el archivo se descarta al no referenciarse ningun simbolo.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
