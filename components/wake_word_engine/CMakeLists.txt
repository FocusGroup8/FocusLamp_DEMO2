idf_component_register(
    SRCS "src/wake_word_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr audio_bridge esp_event
)
