idf_component_register(
    SRCS "src/game_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_service lcd_service servo_service arm_service touch_service
)