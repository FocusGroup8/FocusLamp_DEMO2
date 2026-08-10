/*
 * app_event_handler.c - Global event handler implementation for FocusLamp
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "app_event_handler.h"
#include "event_bus.h"
#include "event_def.h"
#include "command_parser.h"
#include "status_packet.h"
#include "data_type.h"
#include "system_config.h"
#include "app_state.h"
#include "device_state.h"
#include "lcd_service.h"
#include "led_service.h"
#include "servo_service.h"
#include "power_service.h"
#include "radar_module.h"
#include "touch_driver.h"

#include "focus_app.h"
#include "lighting_app.h"
#include "companion_app.h"
#include "game_app.h"
#include "arm_action_app.h"
#include "music_rhythm_app.h"

/* MCP remote control handler (now driven by WebSocket /mcp via ws_manager) */
#include "app_mcp_handler.h"

static const char *TAG = "app_event_handler";

/* ===================== System Event Handlers ===================== */

static void on_sys_startup_complete(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "System startup complete");

    app_state_manager_set_state(APP_STATE_IDLE);
    lighting_app_start();
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);
}

static void on_sys_error(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGE(TAG, "System error occurred");

    app_state_manager_set_state(APP_STATE_ERROR);
    led_service_turn_off();
}

static void on_sys_watchdog_triggered(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGW(TAG, "Watchdog triggered");

    event_bus_publish_simple(EV_SYS_ERROR);
    app_state_manager_set_state(APP_STATE_ERROR);
}

static void on_sys_shutdown(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "System shutdown requested");

    focus_app_stop();
    lighting_app_stop();
    companion_app_stop();
    game_app_stop();
    music_rhythm_app_stop();
    arm_action_app_stop();

    led_service_turn_off();
    lcd_service_sleep();
}

/* ===================== Power Event Handlers ===================== */

static void on_power_low_battery(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGW(TAG, "Low battery - entering power saving mode");

    app_state_manager_set_state(APP_STATE_IDLE);
}

static void on_power_charging(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "Charging started");

    lcd_service_expression_set(LCD_EXPRESSION_HAPPY);
}

static void on_power_charged(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "Charging complete");

    led_service_set_color(0, 255, 0);
    led_service_show_blink(LED_BLINK_MODE_SLOW, 0);
}

/* ===================== Touch Event Handlers ===================== */

static void sync_touch_event_to_state(event_t *event, touch_event_t touch_evt)
{
    if (event == NULL || event->data == NULL || event->data_size != sizeof(uint8_t)) {
        return;
    }
    uint8_t point = *(const uint8_t *)event->data;
    device_state_set_key(point, (uint8_t)touch_evt, event->timestamp, touch_driver_get_active_points());
}

static void on_touch_a_single_click(event_t *event, void *context)
{
    (void)context;
    sync_touch_event_to_state(event, TOUCH_EVENT_SINGLE_CLICK);
    ESP_LOGI(TAG, "Touch A single click - switching LCD page");
    lcd_service_page_next();
}

static app_state_t get_next_cyclic_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:         return APP_STATE_LIGHTING;
    case APP_STATE_LIGHTING:     return APP_STATE_FOCUS;
    case APP_STATE_FOCUS:        return APP_STATE_COMPANION;
    case APP_STATE_COMPANION:    return APP_STATE_ARM_ACTION;
    case APP_STATE_ARM_ACTION:   return APP_STATE_MUSIC_RHYTHM;
    case APP_STATE_MUSIC_RHYTHM: return APP_STATE_GAME;
    case APP_STATE_GAME:         return APP_STATE_VOICE;
    case APP_STATE_VOICE:        return APP_STATE_IDLE;
    default:                     return APP_STATE_IDLE;
    }
}

static void on_touch_a_double_click(event_t *event, void *context)
{
    (void)context;
    sync_touch_event_to_state(event, TOUCH_EVENT_DOUBLE_CLICK);

    app_state_t current = app_state_manager_get_state();
    app_state_t next = get_next_cyclic_state(current);

    ESP_LOGI(TAG, "Double-click detected - switching mode from %d to %d", current, next);

    app_state_manager_set_state(next);
}

static void on_touch_a_long_press(event_t *event, void *context)
{
    (void)context;
    sync_touch_event_to_state(event, TOUCH_EVENT_LONG_PRESS);

    ESP_LOGI(TAG, "Long press detected - shutdown sequence");

    power_service_shutdown();
}

static void on_touch_release(event_t *event, void *context)
{
    (void)context;
    sync_touch_event_to_state(event, TOUCH_EVENT_RELEASE);
    ESP_LOGD(TAG, "Touch release detected");
}

/* ===================== Sensor Event Handlers ===================== */

static void on_radar_presence(event_t *event, void *context)
{
    (void)context;
    if (event->data != NULL && event->data_size == sizeof(radar_presence_data_t)) {
        const radar_presence_data_t *pd = (const radar_presence_data_t *)event->data;
        device_state_t state = {0};
        device_state_get(&state);
        device_state_set_radar(pd->is_present,
                               state.radar.heart_rate_bpm,
                               state.radar.breath_rate_bpm,
                               pd->distance_cm,
                               state.radar.hrv_sdnn,
                               state.radar.hrv_rmssd);
        ESP_LOGD(TAG, "Radar presence: %d", pd->is_present);
    }
}

static void on_radar_heart_rate(event_t *event, void *context)
{
    (void)context;
    if (event->data != NULL && event->data_size == sizeof(radar_heart_data_t)) {
        const radar_heart_data_t *hd = (const radar_heart_data_t *)event->data;
        device_state_t state = {0};
        device_state_get(&state);
        device_state_set_radar(state.radar.present,
                               hd->heart_rate,
                               state.radar.breath_rate_bpm,
                               state.radar.distance_cm,
                               state.radar.hrv_sdnn,
                               state.radar.hrv_rmssd);
        ESP_LOGD(TAG, "Radar heart rate: %.1f bpm", hd->heart_rate);
    }
}

static void on_radar_target_range(event_t *event, void *context)
{
    (void)context;
    if (event->data != NULL && event->data_size == sizeof(radar_target_range_data_t)) {
        const radar_target_range_data_t *rd = (const radar_target_range_data_t *)event->data;
        device_state_t state = {0};
        device_state_get(&state);
        device_state_set_radar(state.radar.present,
                               state.radar.heart_rate_bpm,
                               state.radar.breath_rate_bpm,
                               rd->range_cm,
                               state.radar.hrv_sdnn,
                               state.radar.hrv_rmssd);
        ESP_LOGD(TAG, "Radar target range: %.1f cm", rd->range_cm);
    }
}

static void on_radar_breath(event_t *event, void *context)
{
    (void)context;
    if (event->data != NULL && event->data_size == sizeof(radar_breath_data_t)) {
        const radar_breath_data_t *bd = (const radar_breath_data_t *)event->data;
        device_state_t state = {0};
        device_state_get(&state);
        device_state_set_radar(state.radar.present,
                               state.radar.heart_rate_bpm,
                               bd->breath_rate,
                               state.radar.distance_cm,
                               state.radar.hrv_sdnn,
                               state.radar.hrv_rmssd);
        ESP_LOGD(TAG, "Radar breath rate: %.1f bpm", bd->breath_rate);
    }
}

static void on_radar_hrv_ready(event_t *event, void *context)
{
    (void)context;
    if (event->data != NULL && event->data_size == sizeof(radar_hrv_data_t)) {
        const radar_hrv_data_t *hd = (const radar_hrv_data_t *)event->data;
        device_state_t state = {0};
        device_state_get(&state);
        device_state_set_radar(state.radar.present,
                               state.radar.heart_rate_bpm,
                               state.radar.breath_rate_bpm,
                               state.radar.distance_cm,
                               hd->sdnn_ms,
                               hd->rmssd_ms);
        ESP_LOGD(TAG, "Radar HRV: SDNN=%.1f RMSSD=%.1f", hd->sdnn_ms, hd->rmssd_ms);
    }
}

/* ===================== Application Event Handlers ===================== */

static void stop_app_for_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_FOCUS:
        focus_app_stop();
        break;
    case APP_STATE_LIGHTING:
    case APP_STATE_IDLE:
        lighting_app_stop();
        break;
    case APP_STATE_COMPANION:
        companion_app_stop();
        break;
    case APP_STATE_ARM_ACTION:
        arm_action_app_stop();
        break;
    case APP_STATE_MUSIC_RHYTHM:
        music_rhythm_app_stop();
        break;
    case APP_STATE_GAME:
        game_app_stop();
        break;
    default:
        break;
    }
}

static void start_app_for_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
        lighting_app_start();
        break;
    case APP_STATE_LIGHTING:
        lighting_app_start();
        break;
    case APP_STATE_FOCUS:
        focus_app_start(30);
        break;
    case APP_STATE_COMPANION:
        companion_app_start();
        break;
    case APP_STATE_ARM_ACTION:
        arm_action_app_play(ARM_ACTION_WAVE);
        break;
    case APP_STATE_MUSIC_RHYTHM:
        music_rhythm_app_start();
        break;
    case APP_STATE_GAME:
        game_app_start(GAME_ID_ROCK_PAPER_SCISSORS);
        break;
    case APP_STATE_SLEEP:
        led_service_turn_off();
        lcd_service_sleep();
        break;
    case APP_STATE_ERROR:
        led_service_turn_off();
        break;
    default:
        break;
    }
}

static void set_expression_for_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
    case APP_STATE_LIGHTING:
        lcd_service_expression_set(LCD_EXPRESSION_NORMAL);
        break;
    case APP_STATE_FOCUS:
        lcd_service_expression_set(LCD_EXPRESSION_NORMAL);
        break;
    case APP_STATE_SLEEP:
    case APP_STATE_ERROR:
        lcd_service_expression_set(LCD_EXPRESSION_SLEEPY);
        break;
    case APP_STATE_COMPANION:
    case APP_STATE_MUSIC_RHYTHM:
        lcd_service_expression_set(LCD_EXPRESSION_HAPPY);
        break;
    case APP_STATE_ARM_ACTION:
    case APP_STATE_GAME:
        lcd_service_expression_set(LCD_EXPRESSION_SURPRISED);
        break;
    default:
        lcd_service_expression_set(LCD_EXPRESSION_NORMAL);
        break;
    }
}

static void set_led_for_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
    case APP_STATE_LIGHTING:
        led_service_set_mode(LED_MODE_WHITE);
        led_service_set_effect(LED_EFFECT_STEADY);
        break;
    case APP_STATE_FOCUS:
        led_service_turn_off();
        break;
    case APP_STATE_COMPANION:
        led_service_set_effect(LED_EFFECT_BREATHING);
        break;
    case APP_STATE_ARM_ACTION:
        led_service_set_effect(LED_EFFECT_RAINBOW);
        break;
    case APP_STATE_MUSIC_RHYTHM:
        led_service_set_effect(LED_EFFECT_MUSIC_RHYTHM);
        break;
    case APP_STATE_GAME:
        led_service_set_effect(LED_EFFECT_BLINKING);
        break;
    case APP_STATE_SLEEP:
    case APP_STATE_ERROR:
        led_service_turn_off();
        break;
    default:
        led_service_turn_off();
        break;
    }
}

static void on_app_mode_changed(event_t *event, void *context)
{
    (void)context;

    if (event == NULL || event->data == NULL || event->data_size != sizeof(uint8_t) * 2) {
        ESP_LOGW(TAG, "Invalid mode change event data");
        return;
    }

    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t old_state = (app_state_t)state_data[0];
    app_state_t new_state = (app_state_t)state_data[1];

    ESP_LOGI(TAG, "Application mode changed: %d -> %d", old_state, new_state);

    stop_app_for_state(old_state);
    start_app_for_state(new_state);
    set_expression_for_state(new_state);
    set_led_for_state(new_state);
}

static void on_app_focus_timer_done(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "Focus timer completed");

    app_state_manager_set_state(APP_STATE_IDLE);
}

/* ===================== Command Event Handlers ===================== */

static void on_command_received(event_t *event, void *context)
{
    (void)context;

    if (event == NULL || event->data == NULL ||
        event->data_size < offsetof(command_event_data_t, params)) {
        ESP_LOGW(TAG, "Invalid command event data");
        return;
    }

    const command_event_data_t *cmd = (const command_event_data_t *)event->data;

    ESP_LOGI(TAG, "Command received: 0x%04X (seq=%u, len=%u)",
             cmd->cmd_type, cmd->sequence, cmd->param_len);

    switch (cmd->cmd_type) {
        /* === Power Commands === */
        case CMD_POWER_ON:
            power_service_set_power_mode(POWER_MODE_NORMAL);
            break;
        case CMD_POWER_OFF:
            power_service_shutdown();
            break;
        case CMD_POWER_RESET:
            ESP_LOGI(TAG, "Reset command received, restarting system");
            esp_restart();
            break;
        case CMD_POWER_STATUS_REQ: {
            power_status_t status = {
                .battery_level = power_service_get_battery_level(),
                .is_charging   = 0,
                .power_state   = (uint8_t)power_service_get_power_mode(),
            };
            ESP_LOGI(TAG, "Power status: battery=%u%% (UART peer removed)", status.battery_level);
            break;
        }

        /* === Light Commands === */
        case CMD_LIGHT_SET_BRIGHTNESS:
            if (cmd->param_len >= 1) {
                led_service_set_brightness(cmd->params[0]);
            }
            break;
        case CMD_LIGHT_SET_COLOR_TEMP:
            if (cmd->param_len >= 1) {
                led_service_set_mode((cmd->params[0] < 128) ? LED_MODE_WARM : LED_MODE_WHITE);
            }
            break;
        case CMD_LIGHT_SET_COLOR_RGB:
            if (cmd->param_len >= 3) {
                led_service_set_color(cmd->params[0], cmd->params[1], cmd->params[2]);
            }
            break;
        case CMD_LIGHT_SET_MODE:
            if (cmd->param_len >= 1) {
                led_service_set_mode((led_mode_t)cmd->params[0]);
            }
            break;
        case CMD_LIGHT_TOGGLE:
            event_bus_publish_simple(EV_LIGHT_TOGGLE);
            break;

        /* === Servo Commands === */
        case CMD_SERVO_SET_POSITION:
            if (cmd->param_len >= 3) {
                uint8_t servo_id = cmd->params[0];
                uint16_t position = ((uint16_t)cmd->params[1] << 8) | cmd->params[2];
                servo_service_set_position(servo_id, position);
            }
            break;
        case CMD_SERVO_SET_SPEED:
            if (cmd->param_len >= 3) {
                uint8_t servo_id = cmd->params[0];
                uint16_t speed = ((uint16_t)cmd->params[1] << 8) | cmd->params[2];
                servo_service_set_speed(servo_id, speed);
            }
            break;
        case CMD_SERVO_STOP:
            servo_service_disable();
            break;
        case CMD_SERVO_STATUS_REQ: {
            ESP_LOGI(TAG, "Servo status request ignored (UART peer removed)");
            break;
        }

        /* === Arm Commands === */
        case CMD_ARM_START_SEQUENCE:
            arm_service_start_action();
            break;
        case CMD_ARM_STOP_SEQUENCE:
            arm_service_stop_action();
            break;
        case CMD_ARM_PAUSE_SEQUENCE:
            event_bus_publish_simple(EV_ARM_SEQUENCE_PAUSE);
            break;
        case CMD_ARM_EMERGENCY_STOP:
            arm_service_stop_action();
            break;
        case CMD_ARM_SET_SPEED:
            if (cmd->param_len >= sizeof(float)) {
                float multiplier;
                memcpy(&multiplier, cmd->params, sizeof(float));
                arm_service_set_speed_multiplier(multiplier);
            }
            break;

        /* === Audio Commands (DISABLED: no audio hardware) === */
        case CMD_AUDIO_PLAY:
        case CMD_AUDIO_STOP:
        case CMD_AUDIO_SET_VOLUME:
        case CMD_AUDIO_NEXT_TRACK:
            ESP_LOGW(TAG, "Audio command 0x%04X ignored (audio hardware disabled)", cmd->cmd_type);
            break;

        /* === System Commands === */
        case CMD_SYS_GET_STATUS: {
            device_state_t state = {0};
            device_state_get(&state);
            ESP_LOGI(TAG, "System status requested (UART peer removed)");
            break;
        }
        case CMD_SYS_SET_MODE:
            if (cmd->param_len >= 1) {
                app_state_manager_set_state((app_state_t)cmd->params[0]);
            }
            break;
        case CMD_SYS_HEARTBEAT:
            ESP_LOGD(TAG, "Heartbeat command received");
            break;
        case CMD_SYS_SYNC_TIME:
            ESP_LOGI(TAG, "Time sync command received (not implemented)");
            break;
        case CMD_SYS_VERSION_REQ: {
            ESP_LOGI(TAG, "Version info requested (UART peer removed)");
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown command type: 0x%04X", cmd->cmd_type);
            break;
    }
}

/* ===================== WiFi Comm Event Handlers ===================== */

static void on_wifi_comm_connected(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGI(TAG, "WiFi inter-device connection established");

    /* Notify system that remote peer is reachable */
    event_bus_publish_simple(EV_SYS_STATE_CHANGED);
}

static void on_wifi_comm_disconnected(event_t *event, void *context)
{
    (void)event;
    (void)context;

    ESP_LOGW(TAG, "WiFi inter-device connection lost");
}

/* Note: WiFi comm data reception is now handled by the WebSocket /mcp DATA
 * handler registered in app_mcp_handler_init() (ws_data_handler). The legacy
 * TCP wifi_comm_module path has been retired in favor of WebSocket transport. */

/* ===================== Handler Registration ===================== */

esp_err_t app_event_handler_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Registering global event handlers...");

    /* === System Events === */
    ret = event_bus_subscribe(EV_SYS_STARTUP_COMPLETE, on_sys_startup_complete, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_SYS_ERROR, on_sys_error, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_SYS_WATCHDOG_TRIGGERED, on_sys_watchdog_triggered, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_SYS_SHUTDOWN, on_sys_shutdown, NULL);
    if (ret != ESP_OK) return ret;

    /* === Power Events === */
    ret = event_bus_subscribe(EV_POWER_LOW_BATTERY, on_power_low_battery, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_POWER_CHARGING, on_power_charging, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_POWER_CHARGED, on_power_charged, NULL);
    if (ret != ESP_OK) return ret;

    /* === Touch Events (Point A defaults) === */
    ret = event_bus_subscribe(EV_TOUCH_A_SINGLE_CLICK, on_touch_a_single_click, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_TOUCH_A_DOUBLE_CLICK, on_touch_a_double_click, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_TOUCH_A_LONG_PRESS, on_touch_a_long_press, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_TOUCH_A_RELEASE, on_touch_release, NULL);
    if (ret != ESP_OK) return ret;

    /* === Application Events === */
    ret = event_bus_subscribe(EV_APP_MODE_CHANGED, on_app_mode_changed, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_APP_FOCUS_TIMER_DONE, on_app_focus_timer_done, NULL);
    if (ret != ESP_OK) return ret;

    /* === Radar Events === */
    ret = event_bus_subscribe(EV_RADAR_PRESENCE, on_radar_presence, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_RADAR_HEART_RATE, on_radar_heart_rate, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_RADAR_TARGET_RANGE, on_radar_target_range, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_RADAR_BREATH, on_radar_breath, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_RADAR_HRV_READY, on_radar_hrv_ready, NULL);
    if (ret != ESP_OK) return ret;

    /* === Command Events === */
    ret = event_bus_subscribe(EV_COMMAND_RECEIVED, on_command_received, NULL);
    if (ret != ESP_OK) return ret;

    /* === WiFi Comm Events === */
    ret = event_bus_subscribe(EV_WIFI_COMM_CONNECTED, on_wifi_comm_connected, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_WIFI_COMM_DISCONNECTED, on_wifi_comm_disconnected, NULL);
    if (ret != ESP_OK) return ret;

    /* EV_WIFI_COMM_DATA_RECEIVED subscription removed: MCP traffic now arrives
     * over WebSocket /mcp and is dispatched by ws_data_handler. */

    ESP_LOGI(TAG, "Global event handlers registered successfully");

    return ESP_OK;
}
