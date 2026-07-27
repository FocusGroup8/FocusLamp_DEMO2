idf_component_register(
    SRCS "src/lcd_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES common bsp driver esp_lcd esp_lcd_gc9107
)
