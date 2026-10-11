idf_component_register(SRCS "src/radar_ld6002.c"
                       INCLUDE_DIRS "include"
                       REQUIRES radar_driver common event_bus esp_timer
                       PRIV_REQUIRES esp_driver_uart)
