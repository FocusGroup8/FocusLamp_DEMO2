idf_component_register(
    SRCS "src/rest_api.c"
    INCLUDE_DIRS "include"
    REQUIRES websocket_manager esp_http_server cjson services drivers lcd_module common status_reporter apps motion_controller tts_bridge
)
