idf_component_register(
    INCLUDE_DIRS "include"
    SRCS
        "command_parser.c"
        "status_packet.c"
    REQUIRES
        common
        event_bus
)