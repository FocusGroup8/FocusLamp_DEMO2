idf_component_register(
    SRCS "src/touch_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus touch_driver
)