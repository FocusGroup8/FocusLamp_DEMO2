idf_component_register(
    SRCS "src/focuslamp_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_xiaozhi mipi_dsi_bridge led_service motion_controller controller_modes lcd_driver
)
