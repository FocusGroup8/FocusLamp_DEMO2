idf_component_register(
    SRCS "src/lcd_module.c" "src/lcd_expression.c" "src/lcd_page_manager.c" "src/lcd_info_display.c"
    INCLUDE_DIRS "include"
    REQUIRES common
             drivers
             bsp
    PRIV_REQUIRES esp_timer
)