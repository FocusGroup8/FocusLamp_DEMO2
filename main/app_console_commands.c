/*
 * app_console_commands.c - Application-level console commands for FocusLamp
 *
 * Registers commands for:
 *   - Power management
 *   - LED control
 *   - Servo / Mechanical arm
 *   - Audio
 *   - Touch sensor
 *   - Communication
 *   - Debug / System info
 *
 * Note: Some commands are stubs because the corresponding hardware
 *       modules have TODO implementations in the new architecture.
 */

#include "app_console_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unified_help.h"
#include "power_service.h"
#include "led_service.h"
#include "servo_service.h"
#include "servo_driver.h"
#include "lcd_module.h"
#include "lcd_driver.h"
#include "touch_driver.h"
#include "sensor_service.h"
#include "radar_module.h"
#include "radar_driver.h"
#if CONFIG_PROJECT_RADAR_LD6002_ENABLE
#include "radar_ld6002.h"
#endif
#include "app_state.h"
#include "device_state.h"
#include "data_type.h"
#include "system_config.h"
#include "param_registry.h"

#include "bsp_gpio.h"
#include "bsp_uart.h"

#include "arm_action_app.h"
#include "lcd_service.h"

/* WiFi */
#include "wifi_manager.h"

/* audio_storage_console — DISABLED (no audio hardware) */

/* Motion controller console commands (declared in motion_controller_console.c) */
extern void register_motion_controller_commands(void);

static const char *TAG = "app_console";

/* ===================== Power Commands ===================== */

static int cmd_power_mode(int argc, char **argv)
{
    if (argc < 2) {
        printf("Current power modes:\n");
        printf("  0 = NORMAL\n");
        printf("  1 = SAVING\n");
        printf("  2 = SLEEP\n");
        printf("Usage: power_mode <0|1|2>\n");
        return 1;
    }

    int mode = atoi(argv[1]);
    esp_err_t ret;

    switch (mode) {
        case 0:
            ret = power_service_set_power_mode(POWER_MODE_NORMAL);
            break;
        case 1:
            ret = power_service_set_power_mode(POWER_MODE_SAVING);
            break;
        case 2:
            ret = power_service_set_power_mode(POWER_MODE_SLEEP);
            break;
        default:
            printf("Invalid mode: %d\n", mode);
            return 1;
    }

    printf("Power mode set: %s\n", esp_err_to_name(ret));
    return 0;
}

static int cmd_power_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Initiating system shutdown...\n");
    power_service_shutdown();
    return 0;
}

static int cmd_power_battery(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t level = power_service_get_battery_level();
    if (level == 0xFF) {
        printf("Battery level: Unknown (not implemented)\n");
    } else {
        printf("Battery level: %d%%\n", level);
    }
    return 0;
}

/* ===================== Touch Commands ===================== */

static int cmd_touch_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Touch test - scanning for 5 seconds...\n");
    printf("Touch points: A(GPIO), B(GPIO), C(GPIO), D(GPIO)\n");
    printf("Press any touch point to see detection...\n\n");

    for (int i = 0; i < 50; i++) {
        touch_driver_scan();
        uint8_t mask = touch_driver_get_active_points();

        if (mask != 0) {
            printf("[%d] Active: ", i * 100);
            for (int j = 0; j < 4; j++) {
                if (mask & (1 << j)) {
                    printf("%c ", 'A' + j);
                }
            }
            printf("\n");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("\nTouch test complete.\n");
    return 0;
}

/* ===================== Audio Commands (DISABLED) ===================== */

#if 0 // DISABLED: no audio hardware

static int cmd_audio_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = audio_driver_init();
    if (ret != ESP_OK) {
        printf("Audio init failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio driver initialized successfully\n");
    return 0;
}

static int cmd_audio_deinit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = audio_driver_deinit();
    if (ret != ESP_OK) {
        printf("Audio deinit failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio driver deinitialized\n");
    return 0;
}

static int cmd_audio_loopback_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = audio_driver_start_loopback();
    if (ret != ESP_OK) {
        printf("Loopback start failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio loopback started (mic -> speaker)\n");
    return 0;
}

static int cmd_audio_loopback_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = audio_driver_stop_loopback();
    if (ret != ESP_OK) {
        printf("Loopback stop failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio loopback stopped\n");
    return 0;
}

static int cmd_audio_set_volume(int argc, char **argv)
{
    if (argc < 2) {
        printf("Current volume: %d/100\n", audio_driver_get_volume());
        printf("Usage: audio_set_volume <0-100>\n");
        return 1;
    }
    int vol = atoi(argv[1]);
    if (vol < 0 || vol > 100) {
        printf("Volume must be 0-100\n");
        return 1;
    }
    esp_err_t ret = audio_driver_set_volume((uint8_t)vol);
    if (ret != ESP_OK) {
        printf("Set volume failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Volume set to %d\n", vol);
    return 0;
}

static int cmd_audio_set_gain(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: audio_set_gain <gain> (0.1 - 10.0)\n");
        return 1;
    }
    float gain = atof(argv[1]);
    esp_err_t ret = audio_driver_set_gain(gain);
    if (ret != ESP_OK) {
        printf("Set gain failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Gain set to %.2f\n", gain);
    return 0;
}

static int cmd_audio_set_noise(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: audio_set_noise <0|1> <threshold>\n");
        printf("Example: audio_set_noise 1 500\n");
        return 1;
    }
    bool enable = atoi(argv[1]) != 0;
    int threshold = atoi(argv[2]);
    esp_err_t ret = audio_driver_set_noise_reduction(enable, threshold);
    if (ret != ESP_OK) {
        printf("Set noise reduction failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Noise reduction %s, threshold: %d\n", enable ? "enabled" : "disabled", threshold);
    return 0;
}

static int cmd_audio_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_driver_status_t status;
    esp_err_t ret = audio_driver_get_status(&status);
    if (ret != ESP_OK) {
        printf("Get status failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    const char *state_str;
    switch (status.state) {
        case AUDIO_STATE_UNINIT:    state_str = "UNINIT"; break;
        case AUDIO_STATE_IDLE:      state_str = "IDLE"; break;
        case AUDIO_STATE_PLAYING:   state_str = "PLAYING"; break;
        case AUDIO_STATE_RECORDING: state_str = "RECORDING"; break;
        case AUDIO_STATE_LOOPBACK:  state_str = "LOOPBACK"; break;
        case AUDIO_STATE_ERROR:     state_str = "ERROR"; break;
        default:                    state_str = "UNKNOWN"; break;
    }

    printf("\n=== Audio Driver Status ===\n");
    printf("State:             %s\n", state_str);
    printf("Sample Rate:       %d Hz\n", status.sample_rate);
    printf("Volume:            %.0f%%\n", status.volume * 100.0f);
    printf("Gain:              %.2f\n", status.gain);
    printf("Noise Reduction:   %s\n", status.noise_reduction_enabled ? "Enabled" : "Disabled");
    printf("Noise Threshold:   %d\n", status.noise_threshold);
    printf("===========================\n\n");
    return 0;
}

static int cmd_audio_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Running audio self-test...\n\n");

    /* Step 1: Init */
    printf("[1/3] Initializing audio driver... ");
    esp_err_t ret = audio_driver_init();
    if (ret != ESP_OK) {
        printf("FAILED: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("OK\n");

    /* Step 2: Check state */
    printf("[2/3] Checking driver state... ");
    audio_state_t state = audio_driver_get_state();
    if (state != AUDIO_STATE_IDLE) {
        printf("UNEXPECTED STATE: %d\n", state);
        return 1;
    }
    printf("OK (IDLE)\n");

    /* Step 3: Get status */
    printf("[3/3] Getting driver status... ");
    audio_driver_status_t drv_status;
    ret = audio_driver_get_status(&drv_status);
    if (ret != ESP_OK) {
        printf("FAILED: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("OK\n\n");

    printf("Audio self-test PASSED\n");
    printf("Volume: %.0f%%, Gain: %.1f, Noise: %s (thresh=%d)\n\n",
           drv_status.volume * 100.0f, drv_status.gain,
           drv_status.noise_reduction_enabled ? "ON" : "OFF",
           drv_status.noise_threshold);

    printf("Available audio commands:\n");
    printf("  audio_status              Show detailed status\n");
    printf("  audio_set_volume <0-100>  Set volume\n");
    printf("  audio_set_gain <0.1-10>   Set gain\n");
    printf("  audio_set_noise <0|1> <t> Set noise reduction\n");
    printf("  audio_loopback_start      Start mic->speaker loopback\n");
    printf("  audio_loopback_stop       Stop loopback\n");
    printf("  audio_tone <freq> [ms]    Play a tone (e.g. 440 3000)\n");
    printf("  audio_tone 0              Stop tone\n");
    printf("  audio_deinit              Deinitialize driver\n");
    return 0;
}

static int cmd_audio_tone(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  audio_tone <freq>         Play continuous tone (e.g. 440)\n");
        printf("  audio_tone <freq> <ms>    Play tone for duration\n");
        printf("  audio_tone 0              Stop tone\n");
        printf("Common frequencies:\n");
        printf("  220  = A3,  440  = A4,  523 = C5 (middle C)\n");
        printf("  660  = E5,  880  = A5,  1000 = 1kHz test tone\n");
        printf("  2000 = 2kHz, 4000 = 4kHz, 8000 = 8kHz\n");
        return 0;
    }

    int freq = atoi(argv[1]);

    if (freq == 0) {
        audio_driver_stop_tone();
        printf("Tone stopped\n");
        return 0;
    }

    if (freq < 20 || freq > 20000) {
        printf("Frequency must be 20-20000 Hz\n");
        return 1;
    }

    uint32_t duration = 0;
    if (argc >= 3) {
        duration = (uint32_t)atoi(argv[2]);
    }

    esp_err_t ret = audio_driver_play_tone((uint16_t)freq, duration);
    if (ret != ESP_OK) {
        printf("Failed to play tone: %s\n", esp_err_to_name(ret));
        return 1;
    }

    if (duration > 0) {
        printf("Playing %d Hz for %lu ms\n", freq, (unsigned long)duration);
    } else {
        printf("Playing %d Hz (continuous). Use 'audio_tone 0' to stop.\n", freq);
    }
    return 0;
}

#endif /* DISABLED: no audio hardware */

/* ===================== LED Commands ===================== */

static int cmd_led_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("LED test commands:\n");
        printf("  led_test color <r> <g> <b>  - Set RGB color (0-255)\n");
        printf("  led_test level <1-5>        - Set brightness level\n");
        printf("  led_test level_up           - Increase brightness level\n");
        printf("  led_test level_down         - Decrease brightness level\n");
        printf("  led_test preset <0-4>       - Set preset color (0=暖白,1=冷白,2=红,3=绿,4=蓝)\n");
        printf("  led_test breath              - Start breathing effect\n");
        printf("  led_test blink [fast|slow]   - Start blinking effect\n");
        printf("  led_test on                  - Turn on LED (white)\n");
        printf("  led_test off                 - Turn off LED\n");
        printf("  led_test status              - Show LED status\n");
        return 0;
    }

    if (strcmp(argv[1], "color") == 0) {
        if (argc < 5) {
            printf("Usage: led_test color <r> <g> <b>\n");
            return 1;
        }
        uint8_t r = (uint8_t)atoi(argv[2]);
        uint8_t g = (uint8_t)atoi(argv[3]);
        uint8_t b = (uint8_t)atoi(argv[4]);
        esp_err_t ret = led_service_set_color(r, g, b);
        if (ret != ESP_OK) {
            printf("Failed to set color: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED color set to RGB(%d, %d, %d)\n", r, g, b);
        return 0;
    }

    if (strcmp(argv[1], "breath") == 0) {
        esp_err_t ret = led_service_set_effect(LED_EFFECT_BREATHING);
        if (ret != ESP_OK) {
            printf("Failed to start breath effect: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED breathing effect started\n");
        return 0;
    }

    if (strcmp(argv[1], "blink") == 0) {
        led_blink_mode_t mode = LED_BLINK_MODE_SLOW;
        if (argc >= 3 && strcmp(argv[2], "fast") == 0) {
            mode = LED_BLINK_MODE_FAST;
        }
        esp_err_t ret = led_service_set_effect(LED_EFFECT_BLINKING);
        if (ret != ESP_OK) {
            printf("Failed to start blink effect: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED blinking effect started (%s)\n", mode == LED_BLINK_MODE_FAST ? "fast" : "slow");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        /* Turn on with white color */
        esp_err_t ret = led_service_set_color(255, 255, 255);
        if (ret != ESP_OK) {
            printf("Failed to turn on LED: %s\n", esp_err_to_name(ret));
            return 1;
        }
        /* Ensure at least level 3 if currently off */
        if (led_service_get_brightness_level() < 3) {
            led_service_set_brightness_level(3);
        }
        printf("LED turned on (white)\n");
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        esp_err_t ret = led_service_turn_off();
        if (ret != ESP_OK) {
            printf("Failed to turn off LED: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED turned off\n");
        return 0;
    }

    if (strcmp(argv[1], "level") == 0) {
        if (argc < 3) {
            printf("Current brightness level: %d/5\n", led_service_get_brightness_level());
            printf("Usage: led_test level <1-5>\n");
            return 1;
        }
        int level = atoi(argv[2]);
        if (level < 1 || level > 5) {
            printf("Level must be 1-5\n");
            return 1;
        }
        esp_err_t ret = led_service_set_brightness_level((uint8_t)level);
        if (ret != ESP_OK) {
            printf("Failed to set level: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED brightness level set to %d/5\n", level);
        return 0;
    }

    if (strcmp(argv[1], "level_up") == 0) {
        esp_err_t ret = led_service_brightness_up();
        if (ret != ESP_OK) {
            printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED brightness level: %d/5\n", led_service_get_brightness_level());
        return 0;
    }

    if (strcmp(argv[1], "level_down") == 0) {
        esp_err_t ret = led_service_brightness_down();
        if (ret != ESP_OK) {
            printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("LED brightness level: %d/5\n", led_service_get_brightness_level());
        return 0;
    }

    if (strcmp(argv[1], "preset") == 0) {
        if (argc < 3) {
            printf("Usage: led_test preset <0-4>\n");
            printf("  0 = 暖白  1 = 冷白  2 = 红色  3 = 绿色  4 = 蓝色\n");
            return 1;
        }
        int preset = atoi(argv[2]);
        if (preset < 0 || preset >= LED_PRESET_COLOR_COUNT) {
            printf("Invalid preset: %d (valid: 0-%d)\n", preset, LED_PRESET_COLOR_COUNT - 1);
            return 1;
        }
        esp_err_t ret = led_service_set_preset_color((led_preset_color_t)preset);
        if (ret != ESP_OK) {
            printf("Failed to set preset: %s\n", esp_err_to_name(ret));
            return 1;
        }
        static const char* preset_names[] = {"暖白", "冷白", "红色", "绿色", "蓝色"};
        printf("LED preset color set to %s (%d)\n", preset_names[preset], preset);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        led_module_snapshot_t snapshot;
        esp_err_t ret = led_service_get_snapshot(&snapshot);
        if (ret != ESP_OK) {
            printf("Failed to get LED status: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("\n=== LED Status ===\n");
        printf("Initialized: %s\n", snapshot.initialized ? "Yes" : "No");
        printf("Brightness:  %d/30 (level %d/5)\n", snapshot.brightness, snapshot.brightness_level);
        printf("Color:       RGB(%d, %d, %d)\n", snapshot.color.red, snapshot.color.green, snapshot.color.blue);
        printf("Mode:        %d\n", snapshot.mode);
        printf("==================\n\n");
        return 0;
    }

    printf("Unknown LED test command: %s\n", argv[1]);
    return 1;
}

/* ===================== Servo Commands ===================== */

static int cmd_servo_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Servo test commands:\n");
        printf("  servo_test lx <bus_id> <pos> [ms]  - Move LX servo by bus ID (1/2/3/5)\n");
        printf("  servo_test move <id> <pos>         - Move servo by index\n");
        printf("  servo_test read <id>               - Read servo position (live)\n");
        printf("  servo_test enable                  - Enable all servos\n");
        printf("  servo_test disable                 - Disable all servos\n");
        printf("  servo_test home [time_ms]          - Move all servos to home\n");
        printf("  servo_test all <em3> <l1> <l2> <l3> <l5> [time_ms] - Set all 5 servos\n");
        printf("  servo_test smooth <em3> <l1> <l2> <l3> <l5> <time_ms> - Smooth move\n");
        printf("  servo_test status                  - Show servo limits/home/status\n");
        printf("  servo_test record start            - Start recording motion\n");
        printf("  servo_test record stop             - Stop recording\n");
        printf("  servo_test play                    - Play recorded motion\n");
        return 0;
    }

    if (strcmp(argv[1], "lx") == 0) {
        if (argc < 4) {
            printf("Usage: servo_test lx <bus_id> <pos> [time_ms]\n");
            printf("  bus_id: 1, 2, 3, 5 (LX servo bus ID)\n");
            return 1;
        }
        uint8_t bus_id = (uint8_t)atoi(argv[2]);
        int16_t pos = (int16_t)atoi(argv[3]);
        uint16_t time_ms = (argc >= 5) ? (uint16_t)atoi(argv[4]) : 500;

        if (bus_id < 1 || bus_id > 5 || bus_id == 4) {
            printf("Invalid bus ID: %d. Valid IDs: 1, 2, 3, 5\n", bus_id);
            return 1;
        }
        if (pos < 0 || pos > 1000) {
            printf("Position out of range (0-1000): %d\n", pos);
            return 1;
        }

        servo_lx_move(bus_id, pos, time_ms);
        printf("LX ID%d moving to position %d (time=%dms)\n", bus_id, pos, time_ms);
        return 0;
    }

    if (strcmp(argv[1], "move") == 0) {
        if (argc < 4) {
            printf("Usage: servo_test move <id> <position>\n");
            printf("  id: 0-%d (servo index)\n", 1);
            return 1;
        }
        uint8_t id = (uint8_t)atoi(argv[2]);
        uint16_t pos = (uint16_t)atoi(argv[3]);
        esp_err_t ret = servo_driver_set_position(id, pos);
        if (ret != ESP_OK) {
            printf("Failed to move servo: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("Servo %d moved to position %d\n", id, pos);
        return 0;
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) {
            printf("Usage: servo_test read <id>\n");
            return 1;
        }
        uint8_t id = (uint8_t)atoi(argv[2]);
        uint16_t pos;
        esp_err_t ret = servo_driver_read_position(id, &pos);
        if (ret != ESP_OK || pos == 0xFFFF) {
            printf("Failed to read servo %d position (%s)\n", id, esp_err_to_name(ret));
            return 1;
        }
        printf("Servo %d position: %d\n", id, pos);
        return 0;
    }

    if (strcmp(argv[1], "torque") == 0) {
        if (argc < 4) {
            printf("Usage: servo_test torque <id> <0|1>\n");
            printf("  id: 0 (EM3), 1 (LX bus ID 1)\n");
            printf("  0=disable, 1=enable\n");
            return 1;
        }
        uint8_t id = (uint8_t)atoi(argv[2]);
        int on = atoi(argv[3]);
        esp_err_t ret;
        if (on) {
            ret = servo_driver_torque_enable(id);
        } else {
            ret = servo_driver_torque_disable(id);
        }
        if (ret != ESP_OK) {
            printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("Servo %d torque %s\n", id, on ? "enabled" : "disabled");
        return 0;
    }

    if (strcmp(argv[1], "enable") == 0) {
        esp_err_t ret = servo_driver_enable();
        if (ret != ESP_OK) {
            printf("Failed to enable servos: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("All servos enabled\n");
        return 0;
    }

    if (strcmp(argv[1], "disable") == 0) {
        esp_err_t ret = servo_driver_disable();
        if (ret != ESP_OK) {
            printf("Failed to disable servos: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("All servos disabled\n");
        return 0;
    }

    if (strcmp(argv[1], "home") == 0) {
        uint16_t time_ms = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 1000;
        esp_err_t ret = servo_service_go_home(time_ms);
        if (ret != ESP_OK) {
            printf("Failed to go home: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("All servos moving home (time=%dms)\n", time_ms);
        return 0;
    }

    if (strcmp(argv[1], "all") == 0) {
        if (argc < 7) {
            printf("Usage: servo_test all <em3> <l1> <l2> <l3> <l5> [time_ms]\n");
            return 1;
        }
        servo_service_positions_t pos = {
            .em3_pos = (int16_t)atoi(argv[2]),
            .lx_pos  = {(int16_t)atoi(argv[3]), (int16_t)atoi(argv[4]),
                        (int16_t)atoi(argv[5]), (int16_t)atoi(argv[6])},
        };
        uint16_t time_ms = (argc >= 8) ? (uint16_t)atoi(argv[7]) : 500;
        esp_err_t ret = servo_service_set_all_positions(&pos, time_ms);
        if (ret != ESP_OK) {
            printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("All servos moving to target (time=%dms)\n", time_ms);
        return 0;
    }

    if (strcmp(argv[1], "smooth") == 0) {
        if (argc < 8) {
            printf("Usage: servo_test smooth <em3> <l1> <l2> <l3> <l5> <time_ms>\n");
            return 1;
        }
        servo_service_positions_t pos = {
            .em3_pos = (int16_t)atoi(argv[2]),
            .lx_pos  = {(int16_t)atoi(argv[3]), (int16_t)atoi(argv[4]),
                        (int16_t)atoi(argv[5]), (int16_t)atoi(argv[6])},
        };
        uint16_t time_ms = (uint16_t)atoi(argv[7]);
        esp_err_t ret = servo_service_smooth_move(&pos, time_ms);
        if (ret != ESP_OK) {
            printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("Smooth move completed\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        servo_service_limits_t limits;
        servo_service_get_limits(&limits);
        servo_service_positions_t home;
        servo_service_get_home_positions(&home);
        printf("\n=== Servo Status ===\n");
        printf("EM3 limits: %d - %d\n", limits.em3_min, limits.em3_max);
        printf("LX  limits: %d - %d\n", limits.lx_min, limits.lx_max);
        printf("Home: EM3=%d LX=[%d,%d,%d,%d]\n",
               home.em3_pos, home.lx_pos[0], home.lx_pos[1], home.lx_pos[2], home.lx_pos[3]);
        printf("====================\n\n");
        return 0;
    }

#if (SERVO_SERVICE_ENABLE == 1)
    if (strcmp(argv[1], "record") == 0) {
        if (argc < 3) {
            printf("Usage: servo_test record start|stop\n");
            return 1;
        }
        if (strcmp(argv[2], "start") == 0) {
            servo_service_start_recording();
            printf("Recording started\n");
            return 0;
        }
        if (strcmp(argv[2], "stop") == 0) {
            servo_service_stop_recording();
            printf("Recording stopped\n");
            return 0;
        }
    }

    if (strcmp(argv[1], "play") == 0) {
        servo_service_start_playback();
        printf("Playback started\n");
        return 0;
    }
#else
    if (strcmp(argv[1], "record") == 0 || strcmp(argv[1], "play") == 0) {
        printf("Recording/playback not enabled (SERVO_SERVICE_ENABLE=0)\n");
        return 1;
    }
#endif

    printf("Unknown servo test command: %s\n", argv[1]);
    return 1;
}

/* ===================== Standalone Servo Commands ===================== */

static int cmd_servo_record(int argc, char **argv)
{
#if (SERVO_SERVICE_ENABLE != 1)
    printf("Recording/playback not enabled (SERVO_SERVICE_ENABLE=0)\n");
    return 1;
#endif
    if (argc < 2) {
        printf("Usage: servo_record <start|stop>\n");
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        servo_service_start_recording();
        printf("Recording started\n");
    } else if (strcmp(argv[1], "stop") == 0) {
        servo_service_stop_recording();
        printf("Recording stopped\n");
    } else {
        printf("Unknown option: %s (use start|stop)\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_servo_play(int argc, char **argv)
{
#if (SERVO_SERVICE_ENABLE != 1)
    printf("Recording/playback not enabled (SERVO_SERVICE_ENABLE=0)\n");
    return 1;
#endif
    (void)argc;
    (void)argv;
    servo_service_start_playback();
    printf("Playback started\n");
    return 0;
}

static int cmd_servo_stop(int argc, char **argv)
{
#if (SERVO_SERVICE_ENABLE != 1)
    printf("Recording/playback not enabled (SERVO_SERVICE_ENABLE=0)\n");
    return 1;
#endif
    (void)argc;
    (void)argv;
    servo_service_status_t status = servo_service_get_status();
    if (status.current_state == SERVO_SERVICE_STATE_RECORDING) {
        servo_service_stop_recording();
        printf("Recording stopped\n");
    } else {
        servo_service_stop_playback();
        printf("Playback stopped\n");
    }
    return 0;
}

static int cmd_servo_slot(int argc, char **argv)
{
#if (SERVO_SERVICE_ENABLE != 1)
    printf("Recording/playback not enabled (SERVO_SERVICE_ENABLE=0)\n");
    return 1;
#endif
    if (argc < 2) {
        printf("Usage: servo_slot <0-2>\n");
        return 1;
    }
    int slot = atoi(argv[1]);
    if (slot < 0 || slot > 2) {
        printf("Invalid slot: %d (valid: 0-2)\n", slot);
        return 1;
    }
    servo_service_switch_slot(slot);
    printf("Switched to slot %d\n", slot);
    return 0;
}

static int cmd_servo_status(int argc, char **argv)
{
#if (SERVO_SERVICE_ENABLE != 1)
    printf("Servo service not enabled (SERVO_SERVICE_ENABLE=0)\n");
    return 1;
#endif
    (void)argc;
    (void)argv;

    servo_service_status_t status = servo_service_get_status();
    const char *state_str = "UNKNOWN";
    switch (status.current_state) {
        case SERVO_SERVICE_STATE_IDLE:       state_str = "IDLE"; break;
        case SERVO_SERVICE_STATE_RECORDING:  state_str = "RECORDING"; break;
        case SERVO_SERVICE_STATE_HAS_DATA:   state_str = "HAS_DATA"; break;
        case SERVO_SERVICE_STATE_PLAYING:    state_str = "PLAYING"; break;
    }

    printf("\n=== Servo Control Status ===\n");
    printf("State:         %s\n", state_str);
    printf("Current Slot:  %d\n", status.current_slot);
    printf("Frame Count:   %d\n", status.frame_count);
    printf("============================\n\n");
    return 0;
}

static int cmd_servo_pos(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n=== Servo Positions (Live Read) ===\n");

    /* Read EM3 (servo 0, bus ID 4) */
    uint16_t em3_pos;
    esp_err_t ret = servo_driver_read_position(0, &em3_pos);
    if (ret == ESP_OK) {
        printf("EM3[ID4]:      %d  (range 0-3000)\n", em3_pos);
    } else {
        printf("EM3[ID4]:      READ FAILED (%s)\n", esp_err_to_name(ret));
    }

    /* Read all LX servos (bus IDs 1, 2, 3, 5) */
    static const uint8_t lx_ids[] = {1, 2, 3, 5};
    for (int i = 0; i < (int)(sizeof(lx_ids) / sizeof(lx_ids[0])); i++) {
        uint16_t pos;
        ret = servo_driver_read_lx_bus_position(lx_ids[i], &pos);
        if (ret == ESP_OK) {
            printf("LX[ID%d]:       %d  (range 0-1000)\n", lx_ids[i], pos);
        } else {
            printf("LX[ID%d]:       READ FAILED\n", lx_ids[i]);
        }
    }

    printf("==================================\n\n");
    return 0;
}

/* ===================== Servo Bus Diagnostics ===================== */

static int cmd_servo_diag(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n=== Servo Bus Diagnostics ===\n\n");

    /* ── EM3 bus test (UART1, OE=GPIO11) ── */
    printf("--- EM3 Bus (UART1, OE=GPIO11) ---\n");

    uint8_t cmd[] = {0xFF, 0xFF, 4, 0x04, 0x02, 0x4A, 0x02, 0};
    /* checksum = ~sum(bytes[2..6]) = ~(4+4+2+74+2) = ~86 = 0xA9 */
    cmd[7] = 0xFF & (~(cmd[2] + cmd[3] + cmd[4] + cmd[5] + cmd[6]));

    printf("TX (%d bytes): ", 8);
    for (int i = 0; i < 8; i++) printf("%02X ", cmd[i]);
    printf("\n");

    /* Flush and send with OE */
    uart_flush(UART_NUM_1);
    printf("OE=HIGH (transmit)\n");
    bsp_gpio_set_level(GPIO_NUM_11, 1);
    esp_err_t ret = bsp_uart_write_bytes(UART_NUM_1, cmd, 8);
    printf("uart_write_bytes returned: %s\n", esp_err_to_name(ret));
    bsp_gpio_set_level(GPIO_NUM_11, 0);
    printf("OE=LOW (receive)\n");

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t rx[32];
    int n = bsp_uart_read_bytes(UART_NUM_1, rx, sizeof(rx), pdMS_TO_TICKS(100));
    printf("RX (%d bytes): ", n > 0 ? n : 0);
    for (int i = 0; i < n && i < 32; i++) printf("%02X ", rx[i]);
    printf("%s\n", n > 0 ? "" : "(no response)");

    /* ── Compare: LX bus test (UART2, OE=GPIO4) for reference ── */
    printf("\n--- LX Bus (UART2, OE=GPIO4) ---\n");

    uint8_t lx_cmd[] = {0x55, 0x55, 1, 0x03, 0x1C, 0};
    lx_cmd[5] = 0xFF & (~(lx_cmd[2] + lx_cmd[3] + lx_cmd[4]));

    printf("TX (%d bytes): ", 6);
    for (int i = 0; i < 6; i++) printf("%02X ", lx_cmd[i]);
    printf("\n");

    uart_flush(UART_NUM_2);
    printf("OE=HIGH (transmit)\n");
    bsp_gpio_set_level(GPIO_NUM_4, 1);
    ret = bsp_uart_write_bytes(UART_NUM_2, lx_cmd, 6);
    printf("uart_write_bytes returned: %s\n", esp_err_to_name(ret));
    bsp_gpio_set_level(GPIO_NUM_4, 0);
    printf("OE=LOW (receive)\n");

    vTaskDelay(pdMS_TO_TICKS(20));

    n = bsp_uart_read_bytes(UART_NUM_2, rx, sizeof(rx), pdMS_TO_TICKS(100));
    printf("RX (%d bytes): ", n > 0 ? n : 0);
    for (int i = 0; i < n && i < 32; i++) printf("%02X ", rx[i]);
    printf("%s\n", n > 0 ? "" : "(no response)");

    printf("\n===============================\n");
    return 0;
}

/* ===================== Arm Commands ===================== */

static int cmd_arm_action(int argc, char **argv)
{
    if (argc < 2) {
        printf("Arm action commands:\n");
        printf("  arm_action wave         - Wave gesture\n");
        printf("  arm_action nod          - Nod gesture\n");
        printf("  arm_action shake        - Shake head\n");
        printf("  arm_action home         - Return to home position\n");
        printf("  arm_action disable      - Disable all servos\n");
        return 0;
    }

    if (strcmp(argv[1], "home") == 0) {
        printf("Moving servos to home position...\n");
        servo_service_enable();
        servo_service_go_home(1000);
        printf("Home position set. Use servo_test for precise control.\n");
        return 0;
    }

    if (strcmp(argv[1], "disable") == 0) {
        servo_service_disable();
        printf("All servos disabled (torque released)\n");
        return 0;
    }

    arm_action_id_t action = ARM_ACTION_MAX;
    if (strcmp(argv[1], "wave") == 0) {
        action = ARM_ACTION_WAVE;
    } else if (strcmp(argv[1], "nod") == 0) {
        action = ARM_ACTION_NOD;
    } else if (strcmp(argv[1], "shake") == 0) {
        action = ARM_ACTION_SHAKE;
    } else {
        printf("Unknown arm action: %s\n", argv[1]);
        return 1;
    }

    esp_err_t ret = arm_action_app_play(action);
    if (ret != ESP_OK) {
        printf("Failed to start arm action '%s': %s\n", argv[1], esp_err_to_name(ret));
        return 1;
    }
    printf("Arm action '%s' started\n", argv[1]);
    return 0;
}

/* ===================== Info / Debug Commands ===================== */

static int cmd_system_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n=== System Information ===\n");
    printf("Project:     %s v%d.%d.%d\n", PROJECT_NAME,
           SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR, SYSTEM_VERSION_PATCH);
    printf("Hardware:    %s\n", HARDWARE_VERSION);
    printf("CPU Freq:    %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf("Free heap:   %lu bytes\n", esp_get_free_heap_size());
    printf("Free IRAM:   %lu bytes\n", esp_get_free_internal_heap_size());
    printf("Min heap:    %lu bytes\n", esp_get_minimum_free_heap_size());
    printf("Uptime:      %lld ms\n", esp_timer_get_time() / 1000);

    printf("\n=== Application State ===\n");
    app_state_t state = app_state_manager_get_state();
    static const char *state_names[] = {
        "INIT", "IDLE", "LIGHTING", "FOCUS", "COMPANION",
        "ARM_ACTION", "MUSIC_RHYTHM", "GAME", "VOICE", "SLEEP", "ERROR"
    };
    printf("App state:   %s (%d)\n",
           (state >= 0 && state <= APP_STATE_ERROR) ? state_names[state] : "UNKNOWN",
           state);

    device_state_t ds = {0};
    if (device_state_get(&ds) == ESP_OK) {
        printf("\n=== Unified Device State ===\n");
        printf("Uptime:      %lu ms\n", (unsigned long)ds.uptime_ms);
        printf("Light:       %s L%d B%d R%d G%d B%d\n",
               ds.light.on ? "ON" : "OFF",
               ds.light.level, ds.light.brightness,
               ds.light.r, ds.light.g, ds.light.b);
        printf("Audio:       V%d %s %s\n",
               ds.audio.level,
               ds.audio.muted ? "MUTE" : "",
               ds.audio.playing ? "PLAY" : "");
        printf("Radar:       %s HR%.1f BR%.1f Dist%.1fcm %s\n",
               ds.radar.present ? "PRESENT" : "-",
               ds.radar.heart_rate_bpm,
               ds.radar.breath_rate_bpm,
               ds.radar.distance_cm,
               ds.radar.distance_valid ? "" : "?");
        printf("HRV:         SDNN%.1f RMSSD%.1f\n",
               ds.radar.hrv_sdnn, ds.radar.hrv_rmssd);
        printf("Servo:       %s E%d L%d,%d,%d,%d\n",
               ds.servo.valid ? "OK" : "?",
               ds.servo.em3_pos,
               ds.servo.lx_pos[0], ds.servo.lx_pos[1],
               ds.servo.lx_pos[2], ds.servo.lx_pos[3]);
        printf("Key/Touch:   P%d E%d A%02X\n",
               ds.key.last_point, ds.key.last_event, ds.key.active_points);
    }

    printf("\n");
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Resetting system...\n");
    esp_restart();
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Free heap:      %10lu bytes\n", esp_get_free_heap_size());
    printf("Min free heap:  %10lu bytes\n", esp_get_minimum_free_heap_size());
    printf("Free internal:  %10lu bytes\n", esp_get_free_internal_heap_size());
    return 0;
}

static int cmd_task_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Task list:\n");
    printf("------------------------------\n");
    vTaskList(NULL);  /* Requires INCLUDE_vTaskList = 1 */
    return 0;
}

/* ===================== WiFi Commands ===================== */

static int cmd_wifi_ip(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#if (WIFI_MANAGER_ENABLE == 1)
    wifi_manager_info_t info;
    esp_err_t ret = wifi_manager_get_info(&info);
    if (ret != ESP_OK) {
        printf("Failed to get WiFi info: %s\n", esp_err_to_name(ret));
        return 1;
    }

    if (info.is_connected) {
        printf("WiFi connected:\n");
        printf("  SSID:    %s\n", info.ssid);
        printf("  IP:      %s\n", info.ip);
        printf("  RSSI:    %d dBm\n", info.rssi);
        printf("  Channel: %u\n", info.channel);
    } else {
        printf("WiFi not connected\n");
    }
#else
    printf("WiFi Manager is disabled\n");
#endif
    return 0;
}

/* ===================== Chip Info Command ===================== */

static int cmd_chip_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("\n=== Chip Information ===\n");
    printf("Model:       ");
    switch (chip_info.model) {
        case CHIP_ESP32:    printf("ESP32"); break;
        case CHIP_ESP32S2:  printf("ESP32-S2"); break;
        case CHIP_ESP32S3:  printf("ESP32-S3"); break;
        case CHIP_ESP32C3:  printf("ESP32-C3"); break;
        case CHIP_ESP32C6:  printf("ESP32-C6"); break;
        case CHIP_ESP32H2:  printf("ESP32-H2"); break;
        default:            printf("Unknown (%d)", chip_info.model); break;
    }
    printf("\n");
    printf("Cores:       %d\n", chip_info.cores);
    printf("Revision:   r%d.%d\n", chip_info.revision / 100, chip_info.revision % 100);
    printf("Features:    ");
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH)  printf("Embedded-Flash ");
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN)   printf("WiFi ");
    if (chip_info.features & CHIP_FEATURE_BLE)        printf("BLE ");
    if (chip_info.features & CHIP_FEATURE_BT)         printf("BT ");
    printf("\n");
    printf("Silicon ID:  0x%x\n", chip_info.model);
    printf("\n");
    return 0;
}

/* ===================== Parameter Commands ===================== */

#if 0 // DISABLED: param commands (not registered)
static int cmd_param_get(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: param_get <param_name>\n");
        printf("Use 'param_list' to see all available parameters.\n");
        return 1;
    }

    const char *name = argv[1];
    float value;
    if (!param_get_float(name, &value)) {
        printf("Parameter '%s' not found. Use 'param_list' to see available.\n", name);
        return 1;
    }

    const runtime_param_t *p = param_find(name);
    printf("'%s' = ", name);
    if (p) {
        if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
            printf("%d", (int)value);
        } else {
            printf("%.4f", value);
        }
        if (p->unit[0] != '\0') {
            printf(" %s", p->unit);
        }
        printf("  [%s]", p->category);
        if (p->description[0] != '\0') {
            printf(" - %s", p->description);
        }
    } else {
        printf("%.4f", value);
    }
    printf("\n");
    return 0;
}

static int cmd_param_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: param_set <param_name> <value>\n");
        return 1;
    }

    const char *name = argv[1];
    const char *val_str = argv[2];

    runtime_param_t *p = param_find(name);
    if (p == NULL) {
        printf("Parameter '%s' not found. Use 'param_list' to see available.\n", name);
        return 1;
    }

    float value;
    if (p->type == PARAM_TYPE_BOOL) {
        if (strcasecmp(val_str, "true") == 0 || strcasecmp(val_str, "on") == 0 ||
            strcasecmp(val_str, "1") == 0 || strcasecmp(val_str, "yes") == 0) {
            value = 1.0f;
        } else if (strcasecmp(val_str, "false") == 0 || strcasecmp(val_str, "off") == 0 ||
                   strcasecmp(val_str, "0") == 0 || strcasecmp(val_str, "no") == 0) {
            value = 0.0f;
        } else {
            printf("Invalid bool value: '%s' (use true/false/on/off/1/0)\n", val_str);
            return 1;
        }
    } else {
        char *endptr;
        value = strtof(val_str, &endptr);
        if (endptr == val_str || *endptr != '\0') {
            printf("Invalid number: '%s'\n", val_str);
            return 1;
        }
    }

    float old_val = 0;
    param_get_float(name, &old_val);

    if (!param_set_float(name, value)) {
        if (p->type == PARAM_TYPE_READ_ONLY) {
            printf("'%s' is read-only\n", name);
        } else {
            printf("Value %.4g out of range [%.4g, %.4g] for '%s'\n", value, p->min_val, p->max_val, name);
        }
        return 1;
    }

    printf("'%s': ", name);
    if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
        printf("%d", (int)old_val);
    } else {
        printf("%.4f", old_val);
    }
    printf(" -> ");
    if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
        printf("%d", (int)value);
    } else {
        printf("%.4f", value);
    }
    if (p->unit[0] != '\0') {
        printf(" %s", p->unit);
    }
    printf("\n");
    return 0;
}

static void list_param_callback(const runtime_param_t *param, void *ctx)
{
    int *idx = (int *)ctx;
    float val = 0;
    param_get_float(param->name, &val);
    printf("  [%2d] %-28s ", (*idx)++, param->name);
    if (param->type == PARAM_TYPE_INT || param->type == PARAM_TYPE_BOOL || param->type == PARAM_TYPE_ENUM) {
        printf("%d", (int)val);
    } else {
        printf("%.4f", val);
    }
    if (param->unit[0] != '\0') printf(" %s", param->unit);
    printf("  (%s)", param->type == PARAM_TYPE_INT ? "int" :
           param->type == PARAM_TYPE_FLOAT ? "float" :
           param->type == PARAM_TYPE_BOOL ? "bool" :
           param->type == PARAM_TYPE_ENUM ? "enum" : "ro");
    if (param->type == PARAM_TYPE_READ_ONLY) printf(" [RO]");
    printf("\n");
}

static int cmd_param_list(int argc, char **argv)
{
    const char *category = (argc >= 2) ? argv[1] : NULL;
    if (category != NULL && strcasecmp(category, "all") == 0) {
        category = NULL;
    }

    size_t count = param_registry_count();
    if (count == 0) {
        printf("No parameters registered.\n");
        return 0;
    }

    if (category != NULL) {
        printf("--- Parameters in [%s] ---\n", category);
        int idx = 1;
        param_iterate_category(category, list_param_callback, &idx);
        return 0;
    }

    /* List categories */
    char categories[16][16];
    size_t cat_count = param_list_categories(categories, 16);
    if (cat_count > 1) {
        printf("=== Categories ===\n");
        for (size_t i = 0; i < cat_count; i++) {
            size_t cat_param_count = param_iterate_category(categories[i], NULL, NULL);
            printf("  %-14s (%d params)\n", categories[i], cat_param_count);
        }
        printf("\nUse 'param_list <category>' to see details.\n");
    } else {
        printf("=== All Parameters (%d) ===\n", count);
        int idx = 1;
        param_iterate_category(NULL, list_param_callback, &idx);
    }
    return 0;
}

static int cmd_param_info(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: param_info <param_name>\n");
        return 1;
    }

    const char *name = argv[1];
    const runtime_param_t *p = param_find(name);
    if (p == NULL) {
        printf("Parameter '%s' not found.\n", name);
        return 1;
    }

    float val = 0;
    param_get_float(p->name, &val);

    const char *type_str = p->type == PARAM_TYPE_INT ? "int" :
                           p->type == PARAM_TYPE_FLOAT ? "float" :
                           p->type == PARAM_TYPE_BOOL ? "bool" :
                           p->type == PARAM_TYPE_ENUM ? "enum" : "read-only";

    printf("Parameter: %s\n", p->name);
    printf("  Category:    %s\n", p->category);
    printf("  Description: %s\n", p->description);
    printf("  Type:        %s\n", type_str);
    printf("  Current:     ");
    if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
        printf("%d", (int)val);
    } else {
        printf("%.4f", val);
    }
    if (p->unit[0] != '\0') printf(" %s", p->unit);
    printf("\n");
    printf("  Default:     ");
    if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
        printf("%d", (int)p->default_val);
    } else {
        printf("%.4f", p->default_val);
    }
    if (p->unit[0] != '\0') printf(" %s", p->unit);
    printf("\n");
    printf("  Range:       [");
    if (p->type == PARAM_TYPE_INT || p->type == PARAM_TYPE_BOOL || p->type == PARAM_TYPE_ENUM) {
        printf("%d, %d", (int)p->min_val, (int)p->max_val);
    } else {
        printf("%.4f, %.4f", p->min_val, p->max_val);
    }
    printf("]");
    if (p->unit[0] != '\0') printf(" %s", p->unit);
    printf("\n");
    if (p->type == PARAM_TYPE_READ_ONLY) {
        printf("  Status:      READ-ONLY (cannot be modified at runtime)\n");
    }
    return 0;
}
#endif // DISABLED: param commands

/* ===================== Audio Play Commands (DISABLED) ===================== */

#if 0 // DISABLED: no audio hardware

static int cmd_audio_play(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: audio_play <filename or uri>\n");
        printf("Play an audio file from storage.\n");
        printf("Examples:\n");
        printf("  audio_play /spiffs/audio/alert.wav\n");
        printf("  audio_play file:///spiffs/audio/white_noise.wav\n");
        printf("  audio_play tone://440\n");
        return 1;
    }

    char uri[128];
    if (strncmp(argv[1], "file://", 7) == 0 ||
        strncmp(argv[1], "tone://", 7) == 0 ||
        argv[1][0] == '/') {
        strncpy(uri, argv[1], sizeof(uri) - 1);
        uri[sizeof(uri) - 1] = '\0';
    } else {
        snprintf(uri, sizeof(uri), "file:///spiffs/audio/%s", argv[1]);
    }

    esp_err_t ret = audio_service_play(uri);
    if (ret != ESP_OK) {
        printf("Audio play failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio play started: %s\n", uri);
    return 0;
}

static int cmd_audio_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = audio_service_stop();
    if (ret != ESP_OK) {
        printf("Audio stop failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Audio stopped\n");
    return 0;
}

static int cmd_audio_playlist(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Available audio files (built-in):\n");
    printf("  /spiffs/audio/white_noise.wav\n");
    printf("  /spiffs/audio/alert.wav\n");
    printf("Use 'audio_play <uri>' to play.\n");
    return 0;
}

#endif /* close #if 0 at line 1442: audio commands disabled */

/* ===================== LCD Commands ===================== */

static int cmd_lcd_page(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcd_page <page>\n");
        printf("  Page 0 = Expression, 1 = Info\n");
        printf("Current page: %d\n", lcd_service_page_get_current());
        return 0;
    }
    int page = atoi(argv[1]);
    if (page < 0 || page >= LCD_PAGE_COUNT) {
        printf("Invalid page: %d (valid: 0-%d)\n", page, LCD_PAGE_COUNT - 1);
        return 1;
    }
    esp_err_t ret = lcd_service_page_switch_to((lcd_page_t)page);
    if (ret != ESP_OK) {
        printf("Failed to switch page: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("LCD page switched to %d\n", page);
    return 0;
}

static int cmd_lcd_expr(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcd_expr <expression>\n");
        printf("  Expression: 0=Normal, 1=Happy, 2=Sad, 3=Angry, 4=Surprised, 5=Sleepy\n");
        printf("Current expression: %d\n", lcd_service_expression_get());
        return 0;
    }
    int expr = atoi(argv[1]);
    if (expr < 0 || expr >= LCD_EXPRESSION_COUNT) {
        printf("Invalid expression: %d\n", expr);
        return 1;
    }
    esp_err_t ret = lcd_service_expression_set((lcd_expression_t)expr);
    if (ret != ESP_OK) {
        printf("Failed to set expression: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("LCD expression set to %d\n", expr);
    return 0;
}

static int cmd_lcd_brightness(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcd_brightness <0-100>\n");
        return 0;
    }
    int brightness = atoi(argv[1]);
    if (brightness < 0 || brightness > 100) {
        printf("Invalid brightness: %d (valid: 0-100)\n", brightness);
        return 1;
    }
    esp_err_t ret = lcd_set_brightness((uint8_t)brightness);
    if (ret != ESP_OK) {
        printf("Failed to set brightness: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("LCD brightness set to %d%%\n", brightness);
    return 0;
}

static int cmd_lcd_info(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: lcd_info <field> <value>\n");
        printf("  Fields: title, timer\n");
        return 0;
    }
    const char *field = argv[1];
    const char *value = argv[2];

    esp_err_t ret = ESP_OK;
    if (strcmp(field, "title") == 0) {
        ret = lcd_service_info_set_title(value);
    } else if (strcmp(field, "timer") == 0) {
        uint32_t seconds = (uint32_t)atoi(value);
        ret = lcd_service_info_set_timer(seconds);
    } else {
        printf("Unknown field: %s\n", field);
        return 1;
    }

    if (ret != ESP_OK) {
        printf("Failed to set %s: %s\n", field, esp_err_to_name(ret));
        return 1;
    }
    printf("LCD info %s = %s\n", field, value);
    return 0;
}

static int cmd_lcd_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcd_test <clear|center|buffer|color>\n");
        printf("  lcd_test clear\n");
        printf("  lcd_test center\n");
        printf("  lcd_test buffer\n");
        printf("  lcd_test color <black|white|red|green|blue>\n");
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        lcd_fill_screen(LCD_COLOR_BLACK);
        printf("LCD cleared\n");
        return 0;
    }
    if (strcmp(argv[1], "center") == 0) {
        lcd_fill_screen(LCD_COLOR_BLACK);
        lcd_fill_rect(LCD_WIDTH / 4, LCD_HEIGHT / 4, LCD_WIDTH / 2, LCD_HEIGHT / 2, LCD_COLOR_WHITE);
        printf("LCD direct center block drawn\n");
        return 0;
    }
    if (strcmp(argv[1], "buffer") == 0) {
        lcd_fill_rect_buffer(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COLOR_BLACK);
        lcd_fill_rect_buffer(LCD_WIDTH / 4, LCD_HEIGHT / 4, LCD_WIDTH / 2, LCD_HEIGHT / 2, LCD_COLOR_WHITE);
        lcd_flush_buffer();
        printf("LCD buffer center block flushed\n");
        return 0;
    }
    if (strcmp(argv[1], "color") == 0) {
        if (argc < 3) {
            printf("Usage: lcd_test color <black|white|red|green|blue>\n");
            return 1;
        }
        lcd_color_t color = LCD_COLOR_WHITE;
        if (strcmp(argv[2], "black") == 0) color = LCD_COLOR_BLACK;
        else if (strcmp(argv[2], "white") == 0) color = LCD_COLOR_WHITE;
        else if (strcmp(argv[2], "red") == 0) color = LCD_COLOR_RED;
        else if (strcmp(argv[2], "green") == 0) color = LCD_COLOR_GREEN;
        else if (strcmp(argv[2], "blue") == 0) color = LCD_COLOR_BLUE;
        else {
            printf("Unknown color: %s\n", argv[2]);
            return 1;
        }
        lcd_fill_screen(color);
        printf("LCD filled with %s\n", argv[2]);
        return 0;
    }

    printf("Unknown lcd_test mode: %s\n", argv[1]);
    return 1;
}

/* ===================== LCD Mode Commands ===================== */

static const char *s_task_names[] = {
    "阅读", "学习", "工作", "运动", "休息"
};
static const int s_task_name_count = 5;

static int cmd_lcd_mode(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcd_mode <focus|companion|silent|custom [name]>\n");
        printf("  focus      专注模式 (task page)\n");
        printf("  companion  陪伴模式 (happy expression)\n");
        printf("  silent     静默模式 (sleepy expression)\n");
        printf("  custom <name>  自定义模式 (info page with custom name)\n");
        printf("    name: 1=阅读 2=学习 3=工作 4=运动 5=休息 or English text\n");
        return 0;
    }

    if (strcmp(argv[1], "focus") == 0) {
        lcd_service_mode_set(LCD_MODE_FOCUS);
        printf("Mode: 专注模式\n");
    } else if (strcmp(argv[1], "companion") == 0) {
        lcd_service_mode_set(LCD_MODE_COMPANION);
        printf("Mode: 陪伴模式\n");
    } else if (strcmp(argv[1], "silent") == 0) {
        lcd_service_mode_set(LCD_MODE_SILENT);
        printf("Mode: 静默模式\n");
    } else if (strcmp(argv[1], "custom") == 0) {
        if (argc >= 3) {
            int name_idx = atoi(argv[2]);
            if (name_idx >= 1 && name_idx <= s_task_name_count) {
                lcd_service_mode_set_name(s_task_names[name_idx - 1]);
            } else {
                lcd_service_mode_set_name(argv[2]);
            }
        }
        lcd_service_mode_set(LCD_MODE_CUSTOM);
        printf("Mode: %s\n", lcd_service_mode_get() == LCD_MODE_CUSTOM ? "CUSTOM" : "?");
    } else {
        printf("Unknown mode: %s\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_lcd_task(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  lcd_task set <name> <seconds>  - Start a focus task\n");
        printf("    name: 1=阅读 2=学习 3=工作 4=运动 5=休息 or text\n");
        printf("  lcd_task stop                  - Stop current task\n");
        printf("  lcd_task status                - Show task status\n");
        printf("Example: lcd_task set 1 1500     - 阅读 25 minutes\n");
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            printf("Usage: lcd_task set <name> <seconds>\n");
            return 1;
        }

        const char *task_name = argv[2];
        int name_idx = atoi(argv[2]);
        if (name_idx >= 1 && name_idx <= s_task_name_count) {
            task_name = s_task_names[name_idx - 1];
        }

        uint32_t seconds = (uint32_t)atoi(argv[3]);
        lcd_service_task_start(task_name, seconds);
        printf("Task started: '%s' %" PRIu32 " seconds\n", task_name, seconds);

    } else if (strcmp(argv[1], "stop") == 0) {
        lcd_service_task_stop();
        printf("Task stopped\n");

    } else if (strcmp(argv[1], "status") == 0) {
        if (lcd_service_task_is_running()) {
            printf("Task is running\n");
        } else {
            printf("No task running\n");
        }

    } else {
        printf("Unknown subcommand: %s\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_lcd_diag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("=== LCD Diagnostic ===\n");
    lcd_driver_diag_backlight("console_cmd");
    lcd_driver_diag_spi("console_cmd");
    printf("=== End Diagnostic ===\n");
    return 0;
}

/* ===================== LVGL Commands ===================== */
/* No LVGL UI in this build; map to lcd_service equivalents. */

static int cmd_lvgl_page(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lvgl_page <page>\n");
        printf("  Page 0 = Expression, 1 = Info, 2 = Radar View (mapped to LCD info page)\n");
        return 1;
    }
    int page = atoi(argv[1]);
    lcd_page_t lcd_page = (page == 0) ? LCD_PAGE_EXPRESSION : LCD_PAGE_INFO;
    esp_err_t ret = lcd_service_page_switch_to(lcd_page);
    printf("LCD page switched to %s: %s\n",
           (lcd_page == LCD_PAGE_EXPRESSION) ? "expression" : "info",
           esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : 1;
}

static int cmd_lvgl_expr(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lvgl_expr <expression>\n");
        printf("  Expression: 0=Normal, 1=Happy, 2=Sad, 3=Angry, 4=Surprised, 5=Sleepy\n");
        return 1;
    }
    int expr = atoi(argv[1]);
    if (expr < 0 || expr >= LCD_EXPRESSION_COUNT) {
        printf("Invalid expression: %d\n", expr);
        return 1;
    }
    esp_err_t ret = lcd_service_expression_set((lcd_expression_t)expr);
    printf("LCD expression set to %d: %s\n", expr, esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : 1;
}

static int cmd_lvgl_info(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: lvgl_info <field> <value>\n");
        printf("  Fields: title, timer\n");
        return 1;
    }
    const char *field = argv[1];
    const char *value = argv[2];
    esp_err_t ret = ESP_OK;

    if (strcmp(field, "title") == 0) {
        ret = lcd_service_info_set_title(value);
    } else if (strcmp(field, "timer") == 0) {
        ret = lcd_service_info_set_timer((uint32_t)atoi(value));
    } else {
        printf("Unknown field: %s\n", field);
        return 1;
    }

    printf("LCD info %s = %s: %s\n", field, value, esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : 1;
}

/* ===================== Radar Commands ===================== */

static int cmd_radar_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (radar_module_is_running()) {
        printf("Radar is already running\n");
        return 0;
    }

    esp_err_t ret = radar_module_start();
    if (ret != ESP_OK) {
        printf("Failed to start radar: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Radar started\n");
    return 0;
}

static int cmd_radar_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!radar_module_is_running()) {
        printf("Radar is not running\n");
        return 0;
    }

    esp_err_t ret = radar_module_stop();
    if (ret != ESP_OK) {
        printf("Failed to stop radar: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Radar stopped\n");
    return 0;
}

static int cmd_radar_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    radar_module_snapshot_t snapshot;
    esp_err_t ret = radar_module_get_snapshot(&snapshot);
    if (ret != ESP_OK) {
        printf("Failed to get radar status: %s\n", esp_err_to_name(ret));
        return 1;
    }

    static const char *status_names[] = {
        "IDLE", "RUNNING", "FRAME_READY", "ERROR"
    };
    static const char *parser_result_names[] = {
        "OK", "INCOMPLETE", "INVALID_ARG", "INVALID_LENGTH",
        "HEADER_CHKSUM_ERR", "DATA_CHKSUM_ERR", "BUFFER_OVERFLOW"
    };

    printf("\n=== Radar Status ===\n");
    printf("Status:             %s (%d)\n",
           (snapshot.status >= 0 && snapshot.status <= RADAR_MODULE_STATUS_ERROR)
               ? status_names[snapshot.status] : "UNKNOWN",
           snapshot.status);
    printf("Running:            %s\n", radar_module_is_running() ? "Yes" : "No");
    printf("Initialized:        %s\n", radar_module_is_initialized() ? "Yes" : "No");
    printf("Last parser result: %s\n",
           (snapshot.last_parser_result >= 0 && snapshot.last_parser_result <= RADAR_PARSER_BUFFER_OVERFLOW)
               ? parser_result_names[snapshot.last_parser_result] : "UNKNOWN");
    printf("Total bytes:        %zu\n", snapshot.total_bytes_processed);
    printf("Total frames:       %zu\n", snapshot.total_frames_parsed);
    printf("Parser errors:      %zu\n", snapshot.total_parser_errors);
    printf("Consecutive errors: %zu\n", snapshot.consecutive_errors);
    printf("Has new frame:      %s\n", snapshot.has_new_frame ? "Yes" : "No");
    printf("Last receive:       %lld ms ago\n", radar_module_get_time_since_last_receive_ms());
    printf("In error state:     %s\n", radar_module_is_in_error_state() ? "Yes" : "No");
    printf("Timeout:            %s\n", radar_module_is_timeout() ? "Yes" : "No");
    printf("====================\n\n");
    return 0;
}

static int cmd_rlog(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rlog <module> [level]\n");
        printf("       rlog list\n");
        printf("  Modules: breath, hrv, motion, led, lcd, servo, event_bus\n");
        printf("  Levels: -1(none), 0(error), 1(warn), 2(info), 3(debug), 4(verbose)\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        printf("Radar log modules:\n");
        printf("  breath     - radar_breath_handler\n");
        printf("  hrv        - hrv_analyzer\n");
        printf("  motion     - motion_analyzer\n");
        printf("  led        - led_service\n");
        printf("  lcd        - lcd_service\n");
        printf("  servo      - servo_service\n");
        printf("  event_bus  - event_bus\n");
        return 0;
    }

    if (argc < 3) {
        printf("Error: Level parameter required.\n");
        return 1;
    }

    const char *module = argv[1];
    int level = atoi(argv[2]);
    esp_log_level_t esp_level = ESP_LOG_VERBOSE;
    if (level <= -1) { esp_level = ESP_LOG_NONE; }
    else if (level == 0) { esp_level = ESP_LOG_ERROR; }
    else if (level == 1) { esp_level = ESP_LOG_WARN; }
    else if (level == 2) { esp_level = ESP_LOG_INFO; }
    else if (level == 3) { esp_level = ESP_LOG_DEBUG; }

    static const struct {
        const char *name;
        const char *tag;
    } s_modules[] = {
        {"breath",    "radar_breath"},
        {"hrv",       "hrv_analyzer"},
        {"motion",    "motion_analyzer"},
        {"led",       "led_service"},
        {"lcd",       "lcd_service"},
        {"servo",     "servo_service"},
        {"event_bus", "event_bus"},
    };

    const char *tag = NULL;
    for (size_t i = 0; i < sizeof(s_modules) / sizeof(s_modules[0]); i++) {
        if (strcmp(module, s_modules[i].name) == 0) {
            tag = s_modules[i].tag;
            break;
        }
    }

    if (tag == NULL) {
        printf("Unknown module: %s\n", module);
        return 1;
    }

    esp_log_level_set(tag, esp_level);
    printf("rlog: module=%s tag=%s level=%d\n", module, tag, level);
    return 0;
}

static int cmd_heart_beat_enable(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: heart_beat_enable <0|1>\n");
        printf("  0 = Disable heart rate detection\n");
        printf("  1 = Enable heart rate detection\n");
        return 1;
    }
    int enable = atoi(argv[1]);

    /* Publish a control event for radar heart-rate processing */
    event_t ev = {
        .type = (enable != 0) ? EV_RADAR_HEART_RATE_ENABLE : EV_RADAR_HEART_RATE_DISABLE,
        .data = NULL,
        .data_size = 0,
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);

    printf("Heart rate detection %s\n", enable ? "enabled" : "disabled");
    return 0;
}

/* ===================== HLK-LD6002 Radar Reader Commands ===================== */
#if CONFIG_PROJECT_RADAR_LD6002_ENABLE

static int cmd_radar_ld6002(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: radar_ld6002 <start|stop|status>\n");
        printf("  start   - Init and start the HLK-LD6002 reader task\n");
        printf("  stop    - Stop the reader task\n");
        printf("  status  - Show decoded radar state and parser stats\n");
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (radar_ld6002_is_running()) {
            printf("radar_ld6002 already running\n");
            return 0;
        }
        esp_err_t ret = radar_ld6002_init();
        if (ret != ESP_OK) {
            printf("radar_ld6002_init failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ret = radar_ld6002_start();
        if (ret != ESP_OK) {
            printf("radar_ld6002_start failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("radar_ld6002 started\n");
        return 0;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (!radar_ld6002_is_running()) {
            printf("radar_ld6002 not running\n");
            return 0;
        }
        radar_ld6002_stop();
        printf("radar_ld6002 stopped\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        radar_ld6002_state_t st = {0};
        radar_ld6002_stats_t stats = {0};
        radar_ld6002_get_state(&st);
        radar_ld6002_get_stats(&stats);

        printf("\n=== HLK-LD6002 Radar Reader ===\n");
        printf("Running:        %s\n", radar_ld6002_is_running() ? "Yes" : "No");
        printf("Valid frames:   %lu\n", (unsigned long)stats.valid_frames);
        printf("Checksum errs:  %lu\n", (unsigned long)stats.ck_errors);
        printf("Header ck errs: %lu\n", (unsigned long)stats.hdr_ck_fails);
        printf("SOF found:      %lu\n", (unsigned long)stats.sof_found);
        printf("Heart rate:     ");
        if (st.has_heart) printf("%.0f bpm\n", st.heart_rate); else printf("--\n");
        printf("Breath rate:    ");
        if (st.has_breath) printf("%.1f /min\n", st.breath_rate); else printf("--\n");
        printf("Person present: ");
        if (st.has_presence) printf("%s\n", st.person_present ? "Yes" : "No"); else printf("--\n");
        printf("Range:          ");
        if (st.has_range) printf("%.1f cm (raw=%.2f)\n", st.range_dist / 100.0f, st.range_dist);
        else printf("--\n");
        printf("Position:       ");
        if (st.pos_x != 0 || st.pos_y != 0 || st.pos_z != 0)
            printf("(%.2f, %.2f, %.2f)\n", st.pos_x, st.pos_y, st.pos_z);
        else
            printf("--\n");
        printf("================================\n\n");
        return 0;
    }

    printf("Unknown subcommand: %s (use start|stop|status)\n", argv[1]);
    return 1;
}

#endif /* CONFIG_PROJECT_RADAR_LD6002_ENABLE */

/* ===================== Registration ===================== */

void register_app_commands(void)
{
    /* --- Power Commands --- */
    const esp_console_cmd_t power_mode_cmd = {
        .command = "power_mode",
        .help    = "Set power mode: power_mode <0=normal|1=saving|2=sleep>",
        .func    = &cmd_power_mode,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&power_mode_cmd));

    const esp_console_cmd_t power_shutdown_cmd = {
        .command = "power_off",
        .help    = "Shutdown the system",
        .func    = &cmd_power_shutdown,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&power_shutdown_cmd));

    const esp_console_cmd_t power_battery_cmd = {
        .command = "battery",
        .help    = "Show battery level",
        .func    = &cmd_power_battery,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&power_battery_cmd));

    /* --- Touch Commands --- */
    const esp_console_cmd_t touch_test_cmd = {
        .command = "touch_test",
        .help    = "Test touch sensor input",
        .func    = &cmd_touch_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&touch_test_cmd));

    /* --- Audio Commands --- */
#if 0 // DISABLED: audio command registrations
    const esp_console_cmd_t audio_init_cmd = {
        .command = "audio_init",
        .help    = "Initialize audio driver",
        .func    = &cmd_audio_init,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_init_cmd));

    const esp_console_cmd_t audio_deinit_cmd = {
        .command = "audio_deinit",
        .help    = "Deinitialize audio driver",
        .func    = &cmd_audio_deinit,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_deinit_cmd));

    const esp_console_cmd_t audio_loopback_start_cmd = {
        .command = "audio_loopback_start",
        .help    = "Start mic->speaker loopback",
        .func    = &cmd_audio_loopback_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_loopback_start_cmd));

    const esp_console_cmd_t audio_loopback_stop_cmd = {
        .command = "audio_loopback_stop",
        .help    = "Stop audio loopback",
        .func    = &cmd_audio_loopback_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_loopback_stop_cmd));

    const esp_console_cmd_t audio_set_volume_cmd = {
        .command = "audio_set_volume",
        .help    = "Set volume (0-100)",
        .func    = &cmd_audio_set_volume,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_set_volume_cmd));

    const esp_console_cmd_t audio_set_gain_cmd = {
        .command = "audio_set_gain",
        .help    = "Set gain (0.1-10.0)",
        .func    = &cmd_audio_set_gain,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_set_gain_cmd));

    const esp_console_cmd_t audio_set_noise_cmd = {
        .command = "audio_set_noise",
        .help    = "Set noise reduction: audio_set_noise <0|1> <threshold>",
        .func    = &cmd_audio_set_noise,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_set_noise_cmd));

    const esp_console_cmd_t audio_status_cmd = {
        .command = "audio_status",
        .help    = "Show audio driver status",
        .func    = &cmd_audio_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_status_cmd));

    const esp_console_cmd_t audio_test_cmd = {
        .command = "audio_test",
        .help    = "Test audio output and driver status",
        .func    = &cmd_audio_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_test_cmd));

    const esp_console_cmd_t audio_tone_cmd = {
        .command = "audio_tone",
        .help    = "Play a sine wave tone: audio_tone <freq> [ms]",
        .func    = &cmd_audio_tone,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_tone_cmd));
#endif // DISABLED: audio command registrations

    /* --- LED Commands --- */
    const esp_console_cmd_t led_test_cmd = {
        .command = "led_test",
        .help    = "Test LED effects",
        .func    = &cmd_led_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&led_test_cmd));

    /* --- Servo Commands --- */
    const esp_console_cmd_t servo_test_cmd = {
        .command = "servo_test",
        .help    = "Test servo control",
        .func    = &cmd_servo_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_test_cmd));

    const esp_console_cmd_t servo_record_cmd = {
        .command = "servo_record",
        .help    = "Start/stop recording: servo_record <start|stop>",
        .func    = &cmd_servo_record,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_record_cmd));

    const esp_console_cmd_t servo_play_cmd = {
        .command = "servo_play",
        .help    = "Play recorded servo motion",
        .func    = &cmd_servo_play,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_play_cmd));

    const esp_console_cmd_t servo_stop_cmd = {
        .command = "servo_stop",
        .help    = "Stop servo recording/playback",
        .func    = &cmd_servo_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_stop_cmd));

    const esp_console_cmd_t servo_slot_cmd = {
        .command = "servo_slot",
        .help    = "Switch recording slot: servo_slot <0-2>",
        .func    = &cmd_servo_slot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_slot_cmd));

    const esp_console_cmd_t servo_status_cmd = {
        .command = "servo_status",
        .help    = "Show servo control status (state/slot/frames)",
        .func    = &cmd_servo_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_status_cmd));

    const esp_console_cmd_t servo_pos_cmd = {
        .command = "servo_pos",
        .help    = "Read live servo positions from hardware",
        .func    = &cmd_servo_pos,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_pos_cmd));

    const esp_console_cmd_t servo_diag_cmd = {
        .command = "servo_diag",
        .help    = "Diagnose servo bus communication (EM3 + LX)",
        .func    = &cmd_servo_diag,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&servo_diag_cmd));

    /* --- Arm Commands --- */
    const esp_console_cmd_t arm_action_cmd = {
        .command = "arm_action",
        .help    = "Execute mechanical arm action",
        .func    = &cmd_arm_action,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&arm_action_cmd));

    /* --- System / Debug Commands --- */
    const esp_console_cmd_t sys_info_cmd = {
        .command = "sysinfo",
        .help    = "Show system information",
        .func    = &cmd_system_info,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sys_info_cmd));

    const esp_console_cmd_t wifi_ip_cmd = {
        .command = "wifi_ip",
        .help    = "Show WiFi connection status and IP address",
        .func    = &cmd_wifi_ip,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_ip_cmd));

    const esp_console_cmd_t reset_cmd = {
        .command = "reset",
        .help    = "Reset the system",
        .func    = &cmd_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_cmd));

    const esp_console_cmd_t heap_cmd = {
        .command = "heap",
        .help    = "Show heap memory usage",
        .func    = &cmd_heap,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap_cmd));

    const esp_console_cmd_t task_list_cmd = {
        .command = "tasks",
        .help    = "List all FreeRTOS tasks",
        .func    = &cmd_task_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&task_list_cmd));

    /* --- Register Commands in Unified Help --- */
    unified_help_register_command("power_mode", "Set power mode (0=normal, 1=saving, 2=sleep)",
                                  HELP_CATEGORY_POWER);
    unified_help_register_command("power_off", "Shutdown the system", HELP_CATEGORY_POWER);
    unified_help_register_command("battery", "Show battery level", HELP_CATEGORY_POWER);

    unified_help_register_command("touch_test", "Test touch sensor input", HELP_CATEGORY_TOUCH);

#if 0 // DISABLED: audio help registrations (first set)
    unified_help_register_command("audio_init", "Initialize audio driver", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_deinit", "Deinitialize audio driver", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_loopback_start", "Start mic->speaker loopback", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_loopback_stop", "Stop audio loopback", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_set_volume", "Set volume (0-100)", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_set_gain", "Set gain (0.1-10.0)", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_set_noise", "Set noise reduction", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_status", "Show audio driver status", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_test", "Test audio output and driver status", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_tone", "Play a sine wave tone", HELP_CATEGORY_AUDIO);
#endif // DISABLED: audio help registrations (first set)

    unified_help_register_command("led_test", "Test LED effects", HELP_CATEGORY_LED);

    unified_help_register_command("lcd_test", "LCD direct/buffer test: clear|center|buffer|color",
                                  HELP_CATEGORY_LCD);

    unified_help_register_command("servo_test", "Test servo control", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_record", "Start/stop recording servo motion", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_play", "Play recorded servo motion", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_stop", "Stop servo recording/playback", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_slot", "Switch recording slot (0-2)", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_status", "Show servo control status", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_pos", "Read live servo positions from hardware", HELP_CATEGORY_SERVO);
    unified_help_register_command("servo_diag", "Diagnose servo bus communication (EM3 + LX)", HELP_CATEGORY_SERVO);

    unified_help_register_command("comm_test", "Test dual-board communication",
                                  HELP_CATEGORY_COMM);

    unified_help_register_command("arm_action", "Execute mechanical arm action",
                                  HELP_CATEGORY_ARM);

    unified_help_register_command("sysinfo", "Show system info (heap, uptime, version...)",
                                  HELP_CATEGORY_DEBUG);
    unified_help_register_command("wifi_ip", "Show WiFi connection status and IP address",
                                  HELP_CATEGORY_DEBUG);
    unified_help_register_command("reset", "Reset the system", HELP_CATEGORY_DEBUG);
    unified_help_register_command("heap", "Show heap memory usage", HELP_CATEGORY_DEBUG);
    unified_help_register_command("tasks", "List all FreeRTOS tasks", HELP_CATEGORY_DEBUG);
    unified_help_register_command("chip_info", "Show chip information (model, cores, revision)", HELP_CATEGORY_DEBUG);

#if 0 // DISABLED: audio help registrations (second set)
    unified_help_register_command("audio_play", "Play audio file", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_stop", "Stop audio playback", HELP_CATEGORY_AUDIO);
    unified_help_register_command("audio_playlist", "List audio playlist", HELP_CATEGORY_AUDIO);
#endif // DISABLED: audio help registrations (second set)

    unified_help_register_command("lcd_page", "Switch LCD page", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_expr", "Set LCD expression", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_brightness", "Set LCD brightness (0-100)", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_info", "Show information on LCD", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_diag", "LCD diagnostic: backlight and SPI status", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_mode", "Switch display mode: focus|companion|silent|custom", HELP_CATEGORY_LCD);
    unified_help_register_command("lcd_task", "Manage focus task: set|stop|status", HELP_CATEGORY_LCD);

    unified_help_register_command("lvgl_page", "Switch LVGL page", HELP_CATEGORY_LVGL);
    unified_help_register_command("lvgl_expr", "Set LVGL expression", HELP_CATEGORY_LVGL);
    unified_help_register_command("lvgl_info", "Show LVGL info", HELP_CATEGORY_LVGL);
    unified_help_register_command("lvgl_radar", "Show LVGL radar view", HELP_CATEGORY_LVGL);
    unified_help_register_command("lvgl_chart", "LVGL chart: add data point", HELP_CATEGORY_LVGL);
    unified_help_register_command("lvgl_chart_type", "Set LVGL chart type", HELP_CATEGORY_LVGL);

    unified_help_register_command("radar_start", "Start radar sensor", HELP_CATEGORY_RADAR);
    unified_help_register_command("radar_stop", "Stop radar sensor", HELP_CATEGORY_RADAR);
    unified_help_register_command("radar_status", "Show radar status", HELP_CATEGORY_RADAR);
    unified_help_register_command("rlog", "Set radar log level", HELP_CATEGORY_RADAR);

    unified_help_register_command("heart_beat_enable", "Enable/disable heart beat detection", HELP_CATEGORY_SYSTEM);

    /* --- Audio Storage Commands (DISABLED) --- */
    /* ===== Debug Commands ===== */
    const esp_console_cmd_t chip_info_cmd = {
        .command = "chip_info",
        .help = "Show chip information (model, cores, revision, features)",
        .hint = NULL,
        .func = &cmd_chip_info,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&chip_info_cmd));

#if 0 // DISABLED: audio play commands
    /* ===== Audio Play Commands ===== */
    const esp_console_cmd_t audio_play_cmd = {
        .command = "audio_play",
        .help = "Play audio file",
        .hint = NULL,
        .func = &cmd_audio_play,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_play_cmd));

    const esp_console_cmd_t audio_stop_cmd = {
        .command = "audio_stop",
        .help = "Stop audio playback",
        .hint = NULL,
        .func = &cmd_audio_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_stop_cmd));

    const esp_console_cmd_t audio_playlist_cmd = {
        .command = "audio_playlist",
        .help = "List audio playlist",
        .hint = NULL,
        .func = &cmd_audio_playlist,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_playlist_cmd));
#endif // DISABLED: audio play commands

    /* ===== LCD Commands ===== */
    const esp_console_cmd_t lcd_page_cmd = {
        .command = "lcd_page",
        .help = "Switch LCD page",
        .hint = NULL,
        .func = &cmd_lcd_page,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_page_cmd));

    const esp_console_cmd_t lcd_expr_cmd = {
        .command = "lcd_expr",
        .help = "Set LCD expression",
        .hint = NULL,
        .func = &cmd_lcd_expr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_expr_cmd));

    const esp_console_cmd_t lcd_brightness_cmd = {
        .command = "lcd_brightness",
        .help = "Set LCD brightness (0-100)",
        .hint = NULL,
        .func = &cmd_lcd_brightness,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_brightness_cmd));

    const esp_console_cmd_t lcd_info_cmd = {
        .command = "lcd_info",
        .help = "Show information on LCD",
        .hint = NULL,
        .func = &cmd_lcd_info,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_info_cmd));

    const esp_console_cmd_t lcd_test_cmd = {
        .command = "lcd_test",
        .help = "LCD direct/buffer test: clear|center|buffer|color",
        .hint = NULL,
        .func = &cmd_lcd_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_test_cmd));

    const esp_console_cmd_t lcd_diag_cmd = {
        .command = "lcd_diag",
        .help = "LCD diagnostic: print backlight and SPI status",
        .hint = NULL,
        .func = &cmd_lcd_diag,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_diag_cmd));

    const esp_console_cmd_t lcd_mode_cmd = {
        .command = "lcd_mode",
        .help = "Switch display mode: focus|companion|silent|custom [name]",
        .hint = NULL,
        .func = &cmd_lcd_mode,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_mode_cmd));

    const esp_console_cmd_t lcd_task_cmd = {
        .command = "lcd_task",
        .help = "Manage focus task: set <name> <sec>|stop|status",
        .hint = NULL,
        .func = &cmd_lcd_task,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_task_cmd));

    /* ===== LVGL Commands ===== */
    const esp_console_cmd_t lvgl_page_cmd = {
        .command = "lvgl_page",
        .help = "Switch LVGL page",
        .hint = NULL,
        .func = &cmd_lvgl_page,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lvgl_page_cmd));

    const esp_console_cmd_t lvgl_expr_cmd = {
        .command = "lvgl_expr",
        .help = "Set LVGL expression",
        .hint = NULL,
        .func = &cmd_lvgl_expr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lvgl_expr_cmd));

    const esp_console_cmd_t lvgl_info_cmd = {
        .command = "lvgl_info",
        .help = "Show LVGL info",
        .hint = NULL,
        .func = &cmd_lvgl_info,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lvgl_info_cmd));

    /* ===== Radar Commands ===== */
    const esp_console_cmd_t radar_start_cmd = {
        .command = "radar_start",
        .help = "Start radar sensor",
        .hint = NULL,
        .func = &cmd_radar_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&radar_start_cmd));

    const esp_console_cmd_t radar_stop_cmd = {
        .command = "radar_stop",
        .help = "Stop radar sensor",
        .hint = NULL,
        .func = &cmd_radar_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&radar_stop_cmd));

    const esp_console_cmd_t radar_status_cmd = {
        .command = "radar_status",
        .help = "Show radar status",
        .hint = NULL,
        .func = &cmd_radar_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&radar_status_cmd));

    const esp_console_cmd_t rlog_cmd = {
        .command = "rlog",
        .help = "Set radar log level",
        .hint = NULL,
        .func = &cmd_rlog,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&rlog_cmd));

    const esp_console_cmd_t heart_beat_enable_cmd = {
        .command = "heart_beat_enable",
        .help = "Enable/disable heart beat detection",
        .hint = NULL,
        .func = &cmd_heart_beat_enable,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&heart_beat_enable_cmd));

#if CONFIG_PROJECT_RADAR_LD6002_ENABLE
    const esp_console_cmd_t radar_ld6002_cmd = {
        .command = "radar_ld6002",
        .help = "HLK-LD6002 reader: radar_ld6002 <start|stop|status>",
        .hint = NULL,
        .func = &cmd_radar_ld6002,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&radar_ld6002_cmd));
    unified_help_register_command("radar_ld6002",
                                  "HLK-LD6002 reader control (start|stop|status)",
                                  HELP_CATEGORY_RADAR);
#endif

    /* ===== Motion Controller Commands ===== */
    register_motion_controller_commands();
    unified_help_register_command("motion_init", "Initialize motion controller", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_home", "Execute homing sequence. Use 'async' for background", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_status", "Get motion controller status", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_mode", "Set motion mode (0=Normal, 1=Focus, 2=Company, 3=Show)", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_pose", "Manage poses: <save|restore|get> <slot>", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_action", "Execute preset action: <name> [async]", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_stop", "Stop motion [emergency|soft]", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_resume", "Resume from emergency stop", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_voice", "Execute voice command (simulate)", HELP_CATEGORY_MOTION);
    unified_help_register_command("motion_scene", "Trigger scene action", HELP_CATEGORY_MOTION);

    ESP_LOGI(TAG, "App console commands registered");
}
