idf_component_register(
    SRCS "src/ws2812_module.c"
    INCLUDE_DIRS "include"
    REQUIRES common drivers
    PRIV_REQUIRES esp_timer
)
