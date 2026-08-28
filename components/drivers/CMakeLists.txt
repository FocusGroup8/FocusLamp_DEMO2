idf_component_register(
    SRCS "audio_driver/src/audio_driver.c"
         "lcd_driver/src/lcd_driver.c"
         "lcd_driver/src/lcd_font.c"
         "lcd_driver/src/lcd_font_cn.c"
         "led_driver/src/led_driver.c"
         "light_sensor_driver/src/light_sensor_driver.c"
         "power_driver/src/power_driver.c"
         "radar_driver/src/radar_driver.c"
         "servo_driver/src/servo_driver.c"
         "touch_driver/src/touch_driver.c"
         "uart_light_controller/src/uart_light_controller.c"
    INCLUDE_DIRS "audio_driver/include"
                 "lcd_driver/include"
                 "led_driver/include"
                 "light_sensor_driver/include"
                 "power_driver/include"
                 "radar_driver/include"
                 "servo_driver/include"
                 "touch_driver/include"
                 "uart_light_controller/include"
    REQUIRES common bsp audio_bridge
    PRIV_REQUIRES esp_driver_rmt
                 esp_driver_uart
                 esp_driver_gpio
                 esp_driver_ledc
                 esp_lcd
                 esp_lcd_gc9107
                 esp_driver_spi
)
