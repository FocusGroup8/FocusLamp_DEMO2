idf_component_register(
    SRCS "src/led_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_driver
    PRIV_REQUIRES esp_timer freertos ambient_service
)
