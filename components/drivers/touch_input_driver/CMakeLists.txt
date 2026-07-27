idf_component_register(
    SRCS "src/touch_input_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES common bsp esp_driver_gpio
)