idf_component_register(
    SRCS "src/device_state.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)