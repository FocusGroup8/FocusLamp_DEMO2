cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(mipi_dsi)
option(CAMERA_TCP_TRACE "Instrument lwIP only for the head camera diagnostic build" OFF)
option(CAMERA_TCP_STATE "Provide opt-in, low-rate camera TCP state snapshots (no packet hooks)" ON)
if(CAMERA_TCP_TRACE)
    include(camera_trace/instrument.cmake)
elseif(CAMERA_TCP_STATE)
    idf_component_get_property(state_ws websocket_manager COMPONENT_LIB)
    target_sources(${state_ws} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/camera_trace/camera_tcp_state.c")
    target_include_directories(${state_ws} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/camera_trace")
    target_compile_definitions(${state_ws} PRIVATE CAMERA_TCP_STATE=1)
endif()

# Create and flash LittleFS partition image from littlefs_data directory
littlefs_create_partition_image(storage littlefs_data FLASH_IN_PROJECT)
