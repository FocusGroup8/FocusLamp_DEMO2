idf_component_register(
    SRCS "src/audio_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES common bsp audio_bridge
)
