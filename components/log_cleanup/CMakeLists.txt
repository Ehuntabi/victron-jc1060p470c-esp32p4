idf_component_register(
    SRCS "log_cleanup.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer log camera sd_safe fatfs   # fatfs: f_getfree (hueco libre)
)
