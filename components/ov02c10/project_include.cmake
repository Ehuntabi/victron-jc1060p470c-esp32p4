# Registrar el perfil de ajuste (IPA) del OV02C10 para que esp_ipa lo encuentre.
# Es lo que hace esp_cam_sensor con sus sensores oficiales; aqui el sensor es un
# componente local, asi que lo declaramos nosotros.
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/cfg/ov02c10_default.json" APPEND)
