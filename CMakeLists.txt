cmake_minimum_required(VERSION 3.16)

# Set project name
set(PROJECT_NAME "FocusLamp")

# Include all custom component paths
# Single-component directories (have their own CMakeLists.txt)
# Multi-component directories (contain subdirectories, each a component)
set(EXTRA_COMPONENT_DIRS
    "components/common"
    "components/event_bus"
    "components/protocol"
    "components/bsp"
    "components/drivers"
    "components/services"
    "components/apps"
    "components/modules"
    "components/esp_lcd_gc9107"
)

# Include the ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})