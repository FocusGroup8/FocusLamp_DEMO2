idf_component_register(
    SRCS "src/led_module.c" "src/led_controller.c" "src/led_effects.c" "src/led_presence.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus bsp
    PRIV_REQUIRES esp_timer
)
