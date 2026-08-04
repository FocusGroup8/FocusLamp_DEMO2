idf_component_register(
    SRCS "bsp_gpio.c" "bsp_uart.c" "bsp_spi.c" "bsp_i2s.c" "bsp_adc.c" "bsp_power.c"
    INCLUDE_DIRS "include"
    REQUIRES common drivers
             esp_driver_gpio
             esp_driver_i2s
             esp_driver_spi
             esp_driver_uart
             esp_adc
)