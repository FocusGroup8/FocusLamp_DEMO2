idf_component_register(SRCS "src/servo_driver.c" INCLUDE_DIRS "include"
    REQUIRES common esp_driver_uart esp_driver_gpio freertos)
