idf_component_register(
    SRCS "src/xiaozhi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_xiaozhi esp_event nvs_flash
             task_manager audio_bridge
             services
             event_bus common
)
