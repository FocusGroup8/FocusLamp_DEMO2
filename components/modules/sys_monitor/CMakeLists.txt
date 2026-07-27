idf_component_register(
    SRCS "src/sys_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer heap console event_bus
)
