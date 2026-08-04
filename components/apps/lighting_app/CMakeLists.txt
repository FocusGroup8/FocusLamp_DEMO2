idf_component_register(
    SRCS "src/lighting_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_service
)