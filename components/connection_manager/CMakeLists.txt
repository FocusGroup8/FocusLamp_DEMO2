idf_component_register(SRCS "src/connection_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer wifi_manager websocket_manager
                       PRIV_REQUIRES esp_event)
