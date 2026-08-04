idf_component_register(SRCS "src/websocket_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event esp_netif wifi_manager esp_http_server)
