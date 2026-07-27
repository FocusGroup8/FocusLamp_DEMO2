idf_component_register(SRCS "src/wifi_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event esp_netif nvs_flash esp_wifi event_bus
                       PRIV_REQUIRES espressif__esp_wifi_remote espressif__esp_hosted)
