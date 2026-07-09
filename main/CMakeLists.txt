idf_component_register(SRCS "app_demos.c" "board_init.c" "mipi_dsi_st7701s_test.c" "simple_gui.c" "touch_gui.c" "gesture_recognition.c" "gesture_data_collector.c" "touch_game.c"
                         "audio/i2s_driver.c" "audio/i2s_test.c" "audio/audio_storage.c" "audio/recorder.c" "audio/player.cpp" "audio/audio_manager.c" "audio/audio_test.c"
                       INCLUDE_DIRS "." "audio"
                       REQUIRES esp_driver_ppa
                       PRIV_REQUIRES esp_mm esp_timer)
