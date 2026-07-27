idf_component_register(
    SRCS "src/uart_light_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES common bsp esp_driver_uart
)