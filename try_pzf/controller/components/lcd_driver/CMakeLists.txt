idf_component_register(
    SRCS "src/lcd_driver.c" "src/lcd_font.c" "src/lcd_font_cn.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_lcd esp_lcd_gc9107 esp_driver_spi esp_driver_gpio freertos heap
)
