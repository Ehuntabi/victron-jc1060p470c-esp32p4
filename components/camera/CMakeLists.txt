idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "."
    REQUIRES esp_video esp_cam_sensor driver esp_timer esp_driver_jpeg esp_driver_ppa
    # esp_ipa: solo para el rearme EN CALIENTE del IPA (AWB/AE) tras recuperar el
    # sensor, que es lo que antes obligaba a reiniciar la placa. Es dependencia
    # PRIVADA: no aparece en camera.h.
    PRIV_REQUIRES esp_ipa
)
