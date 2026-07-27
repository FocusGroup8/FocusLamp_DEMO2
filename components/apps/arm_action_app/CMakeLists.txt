idf_component_register(
    SRCS "src/arm_action_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus arm_service
)