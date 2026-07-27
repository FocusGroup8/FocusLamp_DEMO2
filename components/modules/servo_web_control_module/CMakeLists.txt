idf_component_register(
    SRCS
        "src/servo_web_control_module.c"
        "src/servo_web_command_parser.c"
        "src/servo_web_command_executor.c"
        "src/servo_web_status_reporter.c"
        "src/servo_web_http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus drivers servo_control_module json esp_http_server
)