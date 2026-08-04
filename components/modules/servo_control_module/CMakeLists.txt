idf_component_register(
    SRCS "src/servo_control_module.c"
    INCLUDE_DIRS "include"
    REQUIRES common drivers event_bus bsp
)
