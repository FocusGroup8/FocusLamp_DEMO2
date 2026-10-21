idf_component_register(
    SRCS "src/ambient_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common
    PRIV_REQUIRES light_sensor_driver esp_timer freertos
)
