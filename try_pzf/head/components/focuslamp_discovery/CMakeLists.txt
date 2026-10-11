idf_component_register(SRCS "src/focuslamp_discovery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES wifi_manager
                       PRIV_REQUIRES esp_http_client esp_netif esp_timer lwip json)
