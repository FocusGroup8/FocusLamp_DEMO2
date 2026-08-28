idf_component_register(
    SRCS "src/companion_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus led_service lcd_service servo_service arm_service lamp_head_controller tts_bridge sensor_service
)