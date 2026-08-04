idf_component_register(
    SRCS "src/xiaozhi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_xiaozhi esp_event nvs_flash esp_timer task_manager device_controller audio_bridge wake_word_engine mipi_dsi_bridge
)
