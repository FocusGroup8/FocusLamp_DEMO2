idf_component_register(
    SRCS "src/servo_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus servo_driver
)