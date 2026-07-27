idf_component_register(
    SRCS "src/status_reporter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer common lcd_module
)
