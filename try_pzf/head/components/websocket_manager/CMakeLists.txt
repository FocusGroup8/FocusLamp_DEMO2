idf_component_register(SRCS "src/websocket_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event esp_netif wifi_manager esp_http_server)

# OFF is the otherwise-identical baseline for the camera comparison.
option(CAMERA_PREFLIGHT_ENABLED "Drop an image group before writing when its socket is not writable" ON)
set(CAMERA_PREFLIGHT_WAIT_US "0" CACHE STRING "Camera admission wait in microseconds (0 or 15000)")
if(NOT CAMERA_PREFLIGHT_WAIT_US MATCHES "^(0|15000)$")
    message(FATAL_ERROR "CAMERA_PREFLIGHT_WAIT_US must be 0 or 15000")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_PREFLIGHT_WAIT_US=${CAMERA_PREFLIGHT_WAIT_US})
target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_PREFLIGHT_ENABLED=$<BOOL:${CAMERA_PREFLIGHT_ENABLED}>)
