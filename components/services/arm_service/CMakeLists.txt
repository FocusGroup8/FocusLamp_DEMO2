idf_component_register(
    SRCS "src/arm_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus servo_service
)