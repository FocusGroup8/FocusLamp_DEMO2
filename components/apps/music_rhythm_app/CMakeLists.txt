idf_component_register(
    SRCS "src/music_rhythm_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_service arm_service
)