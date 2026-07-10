idf_component_register(SRCS "hello_world_main.c"
                       REQUIRES esp_event esp_netif nvs_flash esp_wifi
                       PRIV_REQUIRES espressif__esp_wifi_remote espressif__esp_hosted
                       INCLUDE_DIRS "")
