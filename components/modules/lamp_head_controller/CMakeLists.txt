idf_component_register(
    SRCS "src/lamp_head_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client common
)
