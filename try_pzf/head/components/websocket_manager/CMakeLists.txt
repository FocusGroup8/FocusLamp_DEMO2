idf_component_register(SRCS "src/websocket_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event esp_netif wifi_manager esp_http_server)

# OFF is the otherwise-identical baseline for the camera comparison.
option(CAMERA_PREFLIGHT_ENABLED "Drop an image group before writing when its socket is not writable" ON)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_PREFLIGHT_ENABLED=$<BOOL:${CAMERA_PREFLIGHT_ENABLED}>)
