idf_component_register(SRCS "src/message_queue.c"
                       INCLUDE_DIRS "include"
                       REQUIRES websocket_manager
                       PRIV_REQUIRES esp_http_server)
