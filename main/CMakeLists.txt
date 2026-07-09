idf_component_register(SRCS "main.c" "board_init.c" "system_manager.c" "system_monitor.c"
                         "display/app_demos.c" "display/simple_gui.c" "display/touch_gui.c" "display/gesture_recognition.c" "display/gesture_data_collector.c" "display/touch_game.c" "display/display_system.c"
                         "audio/audio_system.c" "audio/i2s_driver.c" "audio/i2s_test.c" "audio/audio_storage.c" "audio/recorder.c" "audio/player.cpp" "audio/audio_manager.c" "audio/audio_test.c"
                       INCLUDE_DIRS "." "audio" "display"
                       REQUIRES esp_driver_ppa
                       PRIV_REQUIRES esp_mm esp_timer)
