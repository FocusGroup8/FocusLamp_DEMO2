idf_component_register(SRCS "src/motion_controller.c" "src/action_library.c"
    INCLUDE_DIRS "include" REQUIRES servo_driver common event_bus esp_timer freertos)
