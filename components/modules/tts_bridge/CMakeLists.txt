idf_component_register(
    SRCS "src/tts_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client cjson
)
