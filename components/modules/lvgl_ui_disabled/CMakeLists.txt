# LVGL UI component - requires lvgl library from ESP-IDF component registry
# Install with: idf.py add-dependency "lvgl/lvgl^8.3.0"
# Temporarily disabled SRCS - enable when lvgl is available

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES common
             drivers
             event_bus
    PRIV_REQUIRES freertos
)