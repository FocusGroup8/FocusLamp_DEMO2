cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(mipi_dsi)
option(CAMERA_TCP_TRACE "Instrument lwIP only for the head camera diagnostic build" OFF)
if(CAMERA_TCP_TRACE)
    include(camera_trace/instrument.cmake)
endif()

# Create and flash LittleFS partition image from littlefs_data directory
littlefs_create_partition_image(storage littlefs_data FLASH_IN_PROJECT)
