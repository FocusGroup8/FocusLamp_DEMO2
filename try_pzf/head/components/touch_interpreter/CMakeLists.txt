idf_component_register(
    SRCS "src/touch_interpreter.c"
    INCLUDE_DIRS "include"
    REQUIRES touch_sensor esp_timer
)
