idf_component_register(
    SRCS
        "src/motion_controller.c"
        "src/action_library.c"
        "src/motion_controller_console.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        common
        drivers
        servo_control_module
        event_bus
        console
    PRIV_REQUIRES
        esp_timer
)

# Register console commands
target_compile_options(${COMPONENT_LIB} PRIVATE -DMOTION_CONTROLLER_CONSOLE_ENABLE=1)