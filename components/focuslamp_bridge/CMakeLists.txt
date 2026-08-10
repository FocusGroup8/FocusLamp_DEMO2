idf_component_register(
    SRCS "src/focuslamp_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_xiaozhi mipi_dsi_bridge
)
