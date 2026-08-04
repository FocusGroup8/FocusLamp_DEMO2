idf_component_register(
    SRCS "src/status_receiver.c"
    INCLUDE_DIRS "include"
    REQUIRES websocket_manager esp_http_server cjson esp_timer
)
