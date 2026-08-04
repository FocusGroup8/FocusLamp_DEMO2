idf_component_register(
    SRCS "src/voice_app.c"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus services xiaozhi_manager lamp_head_controller
)
