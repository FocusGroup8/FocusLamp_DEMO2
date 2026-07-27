idf_component_register(
    SRCS "src/audio_service.c"
         "src/audio_service_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES common event_bus audio_driver
)