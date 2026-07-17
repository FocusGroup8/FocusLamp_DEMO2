# Base sources (always compiled)
set(COMP_SRCS "main.c" "board_init.c" "system_manager.c" "system_monitor.c")

# Display sources (conditional on EXAMPLE_ENABLE_DISPLAY)
if(CONFIG_EXAMPLE_ENABLE_DISPLAY)
    list(APPEND COMP_SRCS
        "display/app_demos.c"
        "display/simple_gui.c"
        "display/touch_gui.c"
        "display/gesture_recognition.c"
        "display/gesture_data_collector.c"
        "display/touch_game.c"
        "display/display_system.c"
        "display/lvgl_display.c"
    )
endif()

# Audio sources (conditional on EXAMPLE_ENABLE_AUDIO)
if(CONFIG_EXAMPLE_ENABLE_AUDIO)
    list(APPEND COMP_SRCS
        "audio/audio_system.c"
        "audio/i2s_driver.c"
        "audio/i2s_test.c"
        "audio/audio_storage.c"
        "audio/recorder.c"
        "audio/player.cpp"
        "audio/audio_manager.c"
        "audio/audio_test.c"
    )
endif()

# LED sources (conditional on EXAMPLE_ENABLE_LED)
if(CONFIG_EXAMPLE_ENABLE_LED)
    list(APPEND COMP_SRCS
        "led/led_controller.c"
        "led/led_test.c"
    )
endif()

# Camera sources (conditional on EXAMPLE_ENABLE_CAMERA)
if(CONFIG_EXAMPLE_ENABLE_CAMERA)
    list(APPEND COMP_SRCS
        "camera/camera_controller.c"
        "camera/camera_test.c"
        "camera/camera_preview.c"
    )
endif()

# Include directories
set(COMP_INCLUDE_DIRS "." "led")

if(CONFIG_EXAMPLE_ENABLE_DISPLAY)
    list(APPEND COMP_INCLUDE_DIRS "display")
endif()

if(CONFIG_EXAMPLE_ENABLE_AUDIO)
    list(APPEND COMP_INCLUDE_DIRS "audio")
endif()

if(CONFIG_EXAMPLE_ENABLE_CAMERA)
    list(APPEND COMP_INCLUDE_DIRS "camera")
endif()

# Component dependencies
set(COMP_REQUIRES "esp_driver_ppa" "esp_driver_jpeg")
set(COMP_PRIV_REQUIRES "esp_mm" "esp_timer")

if(CONFIG_EXAMPLE_ENABLE_CAMERA)
    list(APPEND COMP_REQUIRES "espressif__esp_cam_sensor" "espressif__esp_sccb_intf")
    list(APPEND COMP_PRIV_REQUIRES "esp_driver_cam" "esp_driver_isp")
endif()

if(CONFIG_EXAMPLE_ENABLE_DISPLAY)
    list(APPEND COMP_PRIV_REQUIRES "espressif__esp_lvgl_port")
endif()

idf_component_register(SRCS ${COMP_SRCS}
                       INCLUDE_DIRS ${COMP_INCLUDE_DIRS}
                       REQUIRES ${COMP_REQUIRES}
                       PRIV_REQUIRES ${COMP_PRIV_REQUIRES})
