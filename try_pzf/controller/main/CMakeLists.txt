idf_component_register(SRCS "tts_capture.c" "hello_world_main.c" "tts_http_api.c" "device_info_api.c" "peripheral_inputs.c" "mode_http_api.c" "lcd_status_ui.c" "demo_behaviors.c" "local_reminder.c"
                       EMBED_FILES "audio/sedentary_reminder.wav"
                                   "audio/welcome_01.wav" "audio/welcome_02.wav" "audio/welcome_03.wav"
                                   "audio/welcome_04.wav" "audio/welcome_05.wav" "audio/welcome_06.wav"
                                   "audio/welcome_07.wav" "audio/welcome_08.wav" "audio/welcome_09.wav"
                       REQUIRES wifi_manager websocket_manager xiaozhi_manager audio_bridge wake_word_engine connection_manager heartbeat_service message_queue focuslamp_bridge status_receiver focuslamp_discovery led_service motion_controller servo_driver ambient_service radar_ld6002 touch_service controller_modes lcd_driver esp_http_client json)
