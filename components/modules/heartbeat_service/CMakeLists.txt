idf_component_register(SRCS "src/heartbeat_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer websocket_manager
                       PRIV_REQUIRES esp_http_server)
