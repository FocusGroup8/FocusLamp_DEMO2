idf_component_register(
    SRCS "src/led_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_driver
)