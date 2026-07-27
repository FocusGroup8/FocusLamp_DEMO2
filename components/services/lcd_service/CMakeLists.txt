idf_component_register(
    SRCS "src/lcd_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus lcd_driver
)