idf_component_register(
    SRCS "src/power_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus power_driver
)