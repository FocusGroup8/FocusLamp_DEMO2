idf_component_register(
    SRCS "src/light_sensor_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES common bsp esp_adc
)