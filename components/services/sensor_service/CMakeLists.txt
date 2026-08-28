idf_component_register(
    SRCS "src/sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus light_sensor_driver radar_driver
)
