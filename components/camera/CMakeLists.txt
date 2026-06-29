idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "."
    REQUIRES esp_video esp_cam_sensor driver esp_timer
)
