idf_component_register(
    SRCS "alerts.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash log
)
