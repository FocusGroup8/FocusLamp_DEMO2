idf_component_register(
    SRCS "src/radar_ld6002.c"
    INCLUDE_DIRS "include"
    REQUIRES drivers
             common
             event_bus
             radar_module
    PRIV_REQUIRES esp_driver_uart
)
