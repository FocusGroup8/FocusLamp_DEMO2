/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"

#include "xiaozhi_manager.h"
#include "xiaozhi_manager_config.h"
#include "xiaozhi_display_interface.h"

#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"

#include "task_manager.h"
#include "audio_bridge.h"

/* dowm service 层：替代 DEMO2 的 device_controller 模拟层，
 * 让小智 MCP 工具直接驱动真实硬件 */
#include "led_service.h"
#include "servo_service.h"
#include "arm_service.h"
#include "audio_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "data_type.h"

/* The real implementation helpers below (static callbacks, MCP tools, mic
 * control) are compiled even in stub mode because they are not inside the
 * XIAOZHI_MANAGER_ENABLE guard. On the base board the module is disabled
 * (sdkconfig), so these helpers are unused there; suppress the warnings. */
#if (XIAOZHI_MANAGER_ENABLE != 1)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

static const char *TAG = "XIAOZHI_MGR";

static xiaozhi_manager_state_t s_state = XIAOZHI_MANAGER_STATE_IDLE;
static esp_xiaozhi_chat_handle_t s_chat_handle = 0;
static esp_mcp_t *s_mcp_engine = NULL;
static bool s_owns_mcp = false;

static xiaozhi_manager_config_t s_config = {0};
static xiaozhi_display_cb_t s_display_cb = NULL;

/* Microphone state: tracks whether mic capture is active.
 * Mic is started when audio channel opens, stopped when it closes.
 * During TTS playback, mic is paused to avoid echo feedback. */
static bool s_mic_active = false;
static bool s_mic_paused_for_tts = false;

/* Forward declarations for MCP tool callbacks */
static esp_mcp_value_t mcp_tool_notification_speak(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_audio_speaker_set_volume(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_audio_speaker_play_tts(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * Display interface implementation
 *-------------------------------------------------------------*/
esp_err_t xiaozhi_display_register_callback(xiaozhi_display_cb_t cb)
{
    s_display_cb = cb;
    return ESP_OK;
}

esp_err_t xiaozhi_display_notify(xiaozhi_display_event_t event, void *data)
{
    if (s_display_cb) {
        return s_display_cb(event, data);
    }
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi chat audio callback
 *-------------------------------------------------------------*/
static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx)
{
    if (s_config.audio_cb) {
        s_config.audio_cb(data, len, s_config.audio_cb_ctx);
    }
}

/*---------------------------------------------------------------
 * Internal: Microphone callback — OPUS frame from audio_bridge
 *
 * Called by audio_bridge's mic_task for each encoded OPUS frame.
 * Forwards the frame to the xiaozhi server via WebSocket.
 *-------------------------------------------------------------*/
static void xiaozhi_mic_callback(const uint8_t *opus_data, int len, void *ctx)
{
    if (s_chat_handle && !s_mic_paused_for_tts) {
        esp_xiaozhi_chat_send_audio_data(s_chat_handle, (const char *)opus_data, (size_t)len);
    }
}

/*---------------------------------------------------------------
 * Internal: Start/stop microphone capture
 *-------------------------------------------------------------*/
static void mic_start_if_needed(void)
{
    if (s_mic_active || s_mic_paused_for_tts) {
        return;
    }
    esp_err_t ret = audio_bridge_mic_start(xiaozhi_mic_callback, NULL);
    if (ret == ESP_OK) {
        s_mic_active = true;
        ESP_LOGI(TAG, "Microphone capture started");
    } else {
        ESP_LOGE(TAG, "Failed to start microphone: %s", esp_err_to_name(ret));
    }
}

static void mic_stop_if_needed(void)
{
    if (!s_mic_active) {
        return;
    }
    audio_bridge_mic_stop();
    s_mic_active = false;
    s_mic_paused_for_tts = false;
    ESP_LOGI(TAG, "Microphone capture stopped");
}

static void mic_pause_for_tts(void)
{
    if (!s_mic_active || s_mic_paused_for_tts) {
        return;
    }
    /* Stop mic during TTS to avoid echo. The mic_task will be restarted
     * when TTS stops. This is simpler than trying to mute/discard frames. */
    audio_bridge_mic_stop();
    s_mic_paused_for_tts = true;
    s_mic_active = false;
    ESP_LOGI(TAG, "Microphone paused for TTS playback");
}

static void mic_resume_after_tts(void)
{
    if (!s_mic_paused_for_tts) {
        return;
    }
    s_mic_paused_for_tts = false;
    /* Restart mic — audio channel is still open */
    mic_start_if_needed();

    /* Notify server to start listening for user input.
     * Without this, the server does not process incoming audio after TTS ends,
     * causing the conversation to stall after the first reply. */
    if (s_chat_handle) {
        esp_xiaozhi_chat_send_start_listening(s_chat_handle, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    }

    ESP_LOGI(TAG, "Microphone resumed after TTS playback");
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi chat event callback
 *-------------------------------------------------------------*/
static void xiaozhi_event_callback(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *tts_state = (esp_xiaozhi_chat_tts_state_t *)event_data;
        if (tts_state) {
            switch (tts_state->state) {
            case ESP_XIAOZHI_CHAT_TTS_STATE_START:
                s_state = XIAOZHI_MANAGER_STATE_SPEAKING;
                mic_pause_for_tts();
                if (s_config.event_cb) {
                    s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_START, NULL, s_config.event_cb_ctx);
                }
                xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"speaking");
                break;
            case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
                s_state = XIAOZHI_MANAGER_STATE_CONNECTED;
                mic_resume_after_tts();
                if (s_config.event_cb) {
                    s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_STOP, NULL, s_config.event_cb_ctx);
                }
                xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"idle");
                break;
            case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
                if (s_config.event_cb) {
                    s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_SENTENCE, (void *)tts_state->text, s_config.event_cb_ctx);
                }
                xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_TTS_TEXT, (void *)tts_state->text);
                break;
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *text_data = (esp_xiaozhi_chat_text_data_t *)event_data;
        if (text_data && text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) {
            if (s_config.event_cb) {
                s_config.event_cb(XIAOZHI_MANAGER_EVENT_STT_TEXT, (void *)text_data->text, s_config.event_cb_ctx);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *err_info = (esp_xiaozhi_chat_error_info_t *)event_data;
        ESP_LOGE(TAG, "Chat error: code=%d source=%s", err_info->code, err_info->source ? err_info->source : "unknown");
        s_state = XIAOZHI_MANAGER_STATE_ERROR;
        if (s_config.event_cb) {
            s_config.event_cb(XIAOZHI_MANAGER_EVENT_ERROR, err_info, s_config.event_cb_ctx);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD: {
        const char *cmd = (const char *)event_data;
        ESP_LOGI(TAG, "System command: %s", cmd ? cmd : "null");
        /* Application decides whether to execute system commands */
        if (cmd && strcmp(cmd, "reboot") == 0) {
            ESP_LOGW(TAG, "Reboot command received from server - not executing in current build");
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI: {
        const char *emoji = (const char *)event_data;
        ESP_LOGD(TAG, "Emoji: %s", emoji ? emoji : "null");
        break;
    }
    default:
        break;
    }
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi ESP event handler
 *-------------------------------------------------------------*/
static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        s_state = XIAOZHI_MANAGER_STATE_CONNECTED;
        ESP_LOGI(TAG, "Connected to xiaozhi server");
        if (s_config.event_cb) {
            s_config.event_cb(XIAOZHI_MANAGER_EVENT_CONNECTED, NULL, s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"connected");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
        ESP_LOGW(TAG, "Disconnected from xiaozhi server");
        if (s_config.event_cb) {
            s_config.event_cb(XIAOZHI_MANAGER_EVENT_DISCONNECTED, NULL, s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"disconnected");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        s_state = XIAOZHI_MANAGER_STATE_LISTENING;
        ESP_LOGI(TAG, "Audio channel opened");
        mic_start_if_needed();
        if (s_config.event_cb) {
            s_config.event_cb(XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED, NULL, s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"listening");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        s_state = XIAOZHI_MANAGER_STATE_CONNECTED;
        ESP_LOGI(TAG, "Audio channel closed");
        mic_stop_if_needed();
        if (s_config.event_cb) {
            s_config.event_cb(XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED, NULL, s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"idle");
        break;
    }
}

/*---------------------------------------------------------------
 * Internal: MCP tool - notification.speak
 * Proactive TTS injection, called by cloud server
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_notification_speak(const esp_mcp_property_list_t *properties)
{
    const char *message = esp_mcp_property_list_get_property_string(properties, "message");
    int priority = esp_mcp_property_list_get_property_int(properties, "priority");

    ESP_LOGI(TAG, "[MCP] notification.speak: \"%s\" (priority=%d)", message ? message : "null", priority);

    /* The chat module will handle TTS synthesis automatically
     * when the server calls this tool with text content */
    return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - audio_speaker.set_volume
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_audio_speaker_set_volume(const esp_mcp_property_list_t *properties)
{
    int volume = esp_mcp_property_list_get_property_int(properties, "volume");

    ESP_LOGI(TAG, "[MCP] audio_speaker.set_volume: %d", volume);

    audio_bridge_set_volume(volume);
    return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - audio_speaker.play_tts
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_audio_speaker_play_tts(const esp_mcp_property_list_t *properties)
{
    const char *text = esp_mcp_property_list_get_property_string(properties, "text");
    const char *preset_id = esp_mcp_property_list_get_property_string(properties, "preset_id");

    ESP_LOGI(TAG, "[MCP] audio_speaker.play_tts: text=\"%s\" preset_id=\"%s\"",
             text ? text : "null", preset_id ? preset_id : "null");

    /* TODO: integrate with TTS playback when audio hardware available */
    return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Device control MCP tools - 驱动 dowm 真实硬件
 * 替代 DEMO2 device_controller 的模拟实现
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_light_on(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.light.on");
    led_service_set_brightness(200);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_off(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.light.off");
    led_service_turn_off();
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_adjust_brightness(const esp_mcp_property_list_t *properties)
{
    const char *direction = esp_mcp_property_list_get_property_string(properties, "direction");
    ESP_LOGI(TAG, "[MCP] self.light.adjust_brightness: %s", direction ? direction : "null");
    if (direction && strcmp(direction, "up") == 0) {
        led_service_brightness_up();
    } else if (direction && strcmp(direction, "down") == 0) {
        led_service_brightness_down();
    } else {
        return esp_mcp_value_create_bool(false);
    }
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_set_brightness(const esp_mcp_property_list_t *properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
    ESP_LOGI(TAG, "[MCP] self.light.set_brightness: %d", brightness);
    /* brightness 0-100 -> dowm 0-255 */
    led_service_set_brightness((uint8_t)(brightness * 255 / 100));
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_come_here(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.come_here");
    event_t ev = { .type = EV_ARM_SEQUENCE_START, .timestamp = event_bus_get_timestamp() };
    event_bus_publish(&ev);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_go_back(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.go_back");
    /* 先停止当前动作序列（stop_action 内部会释放扭矩） */
    event_t ev = { .type = EV_ARM_SEQUENCE_STOP, .timestamp = event_bus_get_timestamp() };
    event_bus_publish(&ev);
    /* 再使能舵机并回到 home 位置（go_home 非阻塞，下发指令后即返回） */
    servo_service_enable();
    servo_service_go_home(1000);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_dance(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.dance");
    event_t ev = { .type = EV_ARM_SEQUENCE_START, .timestamp = event_bus_get_timestamp() };
    event_bus_publish(&ev);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_speaker_play_song(const esp_mcp_property_list_t *properties)
{
    const char *song_name = esp_mcp_property_list_get_property_string(properties, "song_name");
    ESP_LOGI(TAG, "[MCP] self.speaker.play_song: %s", song_name ? song_name : "null");
    event_t ev = { .type = EV_AUDIO_PLAY, .timestamp = event_bus_get_timestamp() };
    event_bus_publish(&ev);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_speaker_stop(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.speaker.stop");
    event_t ev = { .type = EV_AUDIO_STOP, .timestamp = event_bus_get_timestamp() };
    event_bus_publish(&ev);
    return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: Register all MCP tools
 *-------------------------------------------------------------*/
static esp_err_t register_mcp_tools(void)
{
    esp_err_t ret = ESP_OK;

    /* notification.speak - proactive TTS injection */
    esp_mcp_tool_t *speak_tool = esp_mcp_tool_create(
        "self.notification.speak",
        "主动向用户播报信息，支持不同优先级",
        mcp_tool_notification_speak
    );
    if (!speak_tool) {
        ESP_LOGE(TAG, "Failed to create notification.speak tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *msg_prop = esp_mcp_property_create("message", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(speak_tool, msg_prop);
    esp_mcp_property_t *prio_prop = esp_mcp_property_create_with_range("priority", 0, 3);
    esp_mcp_tool_add_property(speak_tool, prio_prop);
    esp_mcp_add_tool(s_mcp_engine, speak_tool);

    /* audio_speaker.set_volume */
    esp_mcp_tool_t *vol_tool = esp_mcp_tool_create(
        "self.audio_speaker.set_volume",
        "设置音频扬声器音量 (0-100)",
        mcp_tool_audio_speaker_set_volume
    );
    if (!vol_tool) {
        ESP_LOGE(TAG, "Failed to create set_volume tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *vol_prop = esp_mcp_property_create_with_range("volume", 0, 100);
    esp_mcp_tool_add_property(vol_tool, vol_prop);
    esp_mcp_add_tool(s_mcp_engine, vol_tool);

    /* audio_speaker.play_tts */
    esp_mcp_tool_t *tts_tool = esp_mcp_tool_create(
        "self.audio_speaker.play_tts",
        "播放预设文本语音",
        mcp_tool_audio_speaker_play_tts
    );
    if (!tts_tool) {
        ESP_LOGE(TAG, "Failed to create play_tts tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *text_prop = esp_mcp_property_create("text", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(tts_tool, text_prop);
    esp_mcp_property_t *preset_prop = esp_mcp_property_create("preset_id", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(tts_tool, preset_prop);
    esp_mcp_add_tool(s_mcp_engine, tts_tool);

    ESP_LOGI(TAG, "Core MCP tools registered (notification.speak, audio_speaker.set_volume, audio_speaker.play_tts)");

    /* Register sub-module MCP tools */
    ret = task_manager_register_mcp_tools(s_mcp_engine);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register task_manager MCP tools");
        return ret;
    }

    /* self.light.on */
    esp_mcp_tool_t *light_on_tool = esp_mcp_tool_create(
        "self.light.on", "开灯", mcp_tool_light_on);
    if (!light_on_tool) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, light_on_tool);

    /* self.light.off */
    esp_mcp_tool_t *light_off_tool = esp_mcp_tool_create(
        "self.light.off", "关灯", mcp_tool_light_off);
    if (!light_off_tool) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, light_off_tool);

    /* self.light.adjust_brightness */
    esp_mcp_tool_t *adj_tool = esp_mcp_tool_create(
        "self.light.adjust_brightness", "调节灯光亮度 (up/down)", mcp_tool_light_adjust_brightness);
    if (!adj_tool) return ESP_ERR_NO_MEM;
    esp_mcp_tool_add_property(adj_tool, esp_mcp_property_create("direction", ESP_MCP_PROPERTY_TYPE_STRING));
    esp_mcp_add_tool(s_mcp_engine, adj_tool);

    /* self.light.set_brightness */
    esp_mcp_tool_t *set_bright_tool = esp_mcp_tool_create(
        "self.light.set_brightness", "设置灯光亮度 (0-100)", mcp_tool_light_set_brightness);
    if (!set_bright_tool) return ESP_ERR_NO_MEM;
    esp_mcp_tool_add_property(set_bright_tool, esp_mcp_property_create_with_range("brightness", 0, 100));
    esp_mcp_add_tool(s_mcp_engine, set_bright_tool);

    /* self.arm.come_here / go_back / dance */
    esp_mcp_tool_t *arm_come = esp_mcp_tool_create("self.arm.come_here", "机械臂向用户移动", mcp_tool_arm_come_here);
    if (!arm_come) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, arm_come);
    esp_mcp_tool_t *arm_back = esp_mcp_tool_create("self.arm.go_back", "机械臂回初始位置", mcp_tool_arm_go_back);
    if (!arm_back) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, arm_back);
    esp_mcp_tool_t *arm_dance = esp_mcp_tool_create("self.arm.dance", "机械臂跳舞", mcp_tool_arm_dance);
    if (!arm_dance) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, arm_dance);

    /* self.speaker.play_song / stop */
    esp_mcp_tool_t *play_song = esp_mcp_tool_create("self.speaker.play_song", "播放歌曲", mcp_tool_speaker_play_song);
    if (!play_song) return ESP_ERR_NO_MEM;
    esp_mcp_tool_add_property(play_song, esp_mcp_property_create("song_name", ESP_MCP_PROPERTY_TYPE_STRING));
    esp_mcp_add_tool(s_mcp_engine, play_song);
    esp_mcp_tool_t *speaker_stop = esp_mcp_tool_create("self.speaker.stop", "停止播放", mcp_tool_speaker_stop);
    if (!speaker_stop) return ESP_ERR_NO_MEM;
    esp_mcp_add_tool(s_mcp_engine, speaker_stop);

    ESP_LOGI(TAG, "All MCP tools registered successfully (core + task + device)");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
#if (XIAOZHI_MANAGER_ENABLE == 1)

esp_err_t xiaozhi_manager_init(const xiaozhi_manager_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(s_state == XIAOZHI_MANAGER_STATE_IDLE, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    memcpy(&s_config, config, sizeof(s_config));

    esp_err_t ret;

    /* Step 0: Initialize sub-modules */
    ret = task_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init task_manager: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 1: Create MCP engine */
    ret = esp_mcp_create(&s_mcp_engine);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create MCP engine");

    s_owns_mcp = true;

    /* Step 2: Register core MCP tools */
    ret = register_mcp_tools();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MCP tools");
        esp_mcp_destroy(s_mcp_engine);
        s_mcp_engine = NULL;
        return ret;
    }

    /* Step 3: Get device info from server */
    esp_xiaozhi_chat_info_t info = {0};
    ret = esp_xiaozhi_chat_get_info(&info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Device info retrieved: version=%s has_mqtt=%d has_ws=%d",
                 info.current_version ? info.current_version : "N/A",
                 info.has_mqtt_config, info.has_websocket_config);
    } else {
        ESP_LOGW(TAG, "Failed to get device info: %s (continuing anyway)", esp_err_to_name(ret));
    }

    /* Step 4: Configure and init chat */
    esp_xiaozhi_chat_config_t chat_config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    chat_config.audio_callback = xiaozhi_audio_callback;
    chat_config.event_callback = xiaozhi_event_callback;
    chat_config.audio_callback_ctx = NULL;
    chat_config.event_callback_ctx = NULL;
    chat_config.mcp_engine = s_mcp_engine;
    chat_config.owns_mcp_engine = true;
    chat_config.has_mqtt_config = false;       /* 强制使用WebSocket传输，避免UDP不通导致音频数据丢失 */
    chat_config.has_websocket_config = true;

    ret = esp_xiaozhi_chat_init(&chat_config, &s_chat_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init chat: %s", esp_err_to_name(ret));
        esp_xiaozhi_chat_free_info(&info);
        /* MCP engine is owned by chat now if init succeeded partially, skip destroy */
        s_mcp_engine = NULL;
        s_owns_mcp = false;
        return ret;
    }

    /* After chat_init with owns_mcp_engine=true, chat owns the MCP engine */
    s_owns_mcp = false;

    /* Step 5: Register ESP event handler for connection events */
    ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                      xiaozhi_esp_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register ESP event handler: %s", esp_err_to_name(ret));
    }

    esp_xiaozhi_chat_free_info(&info);

    s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
    ESP_LOGI(TAG, "Xiaozhi manager initialized successfully");

    return ESP_OK;
}

esp_err_t xiaozhi_manager_deinit(void)
{
    if (s_state == XIAOZHI_MANAGER_STATE_IDLE) {
        return ESP_OK;
    }

    /* Stop microphone capture first */
    mic_stop_if_needed();

    /* Stop chat if running */
    if (s_chat_handle) {
        esp_xiaozhi_chat_stop(s_chat_handle);
        esp_xiaozhi_chat_deinit(s_chat_handle);
        s_chat_handle = 0;
    }

    /* Deinitialize sub-modules */
    task_manager_deinit();

    s_mcp_engine = NULL;
    s_owns_mcp = false;
    s_state = XIAOZHI_MANAGER_STATE_IDLE;
    s_display_cb = NULL;

    ESP_LOGI(TAG, "Xiaozhi manager deinitialized");
    return ESP_OK;
}

esp_err_t xiaozhi_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    s_state = XIAOZHI_MANAGER_STATE_CONNECTING;
    esp_err_t ret = esp_xiaozhi_chat_start(s_chat_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start chat: %s", esp_err_to_name(ret));
        s_state = XIAOZHI_MANAGER_STATE_ERROR;
        return ret;
    }

    ESP_LOGI(TAG, "Xiaozhi chat session started");
    return ESP_OK;
}

esp_err_t xiaozhi_manager_stop(void)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    esp_err_t ret = esp_xiaozhi_chat_stop(s_chat_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop chat: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
    ESP_LOGI(TAG, "Xiaozhi chat session stopped");
    return ESP_OK;
}

xiaozhi_manager_state_t xiaozhi_manager_get_state(void)
{
    return s_state;
}

esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(wake_word, ESP_ERR_INVALID_ARG, TAG, "Invalid wake word");

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word);
    return esp_xiaozhi_chat_send_wake_word(s_chat_handle, wake_word);
}

esp_err_t xiaozhi_manager_open_audio_channel(void)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    ESP_LOGI(TAG, "Opening audio channel...");
    return esp_xiaozhi_chat_open_audio_channel(s_chat_handle, NULL, NULL, 0);
}

esp_err_t xiaozhi_manager_close_audio_channel(void)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    ESP_LOGI(TAG, "Closing audio channel...");
    return esp_xiaozhi_chat_close_audio_channel(s_chat_handle);
}

esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(data && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid audio data");

    return esp_xiaozhi_chat_send_audio_data(s_chat_handle, data, data_len);
}

esp_mcp_t *xiaozhi_manager_get_mcp_engine(void)
{
    return s_mcp_engine;
}

esp_err_t xiaozhi_manager_speak(const char *text, int priority)
{
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");

    ESP_LOGI(TAG, "Speak request: \"%s\" (priority=%d)", text, priority);

    /* Use the notification.speak MCP tool mechanism.
     * In the xiaozhi protocol, proactive TTS injection is done
     * through the server-side tool call mechanism. */
    if (s_mcp_engine) {
        /* Notify via MCP log message as a signal to the server */
        esp_mcp_notify_log_message(s_mcp_engine, "info", "xiaozhi_manager",
                                   text);
    }

    return ESP_OK;
}

esp_err_t xiaozhi_manager_start_listening(int mode)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    ESP_LOGI(TAG, "Starting listening (mode=%d)", mode);
    return esp_xiaozhi_chat_send_start_listening(s_chat_handle, mode);
}

esp_err_t xiaozhi_manager_stop_listening(void)
{
    ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    ESP_LOGI(TAG, "Stopping listening");
    return esp_xiaozhi_chat_send_stop_listening(s_chat_handle);
}

#else /* XIAOZHI_MANAGER_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t xiaozhi_manager_init(const xiaozhi_manager_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xiaozhi_manager_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
xiaozhi_manager_state_t xiaozhi_manager_get_state(void) { return XIAOZHI_MANAGER_STATE_IDLE; }
esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word) { (void)wake_word; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_open_audio_channel(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_close_audio_channel(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len) { (void)data; (void)data_len; return ESP_ERR_NOT_SUPPORTED; }
esp_mcp_t *xiaozhi_manager_get_mcp_engine(void) { return NULL; }
esp_err_t xiaozhi_manager_speak(const char *text, int priority) { (void)text; (void)priority; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_start_listening(int mode) { (void)mode; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_stop_listening(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* XIAOZHI_MANAGER_ENABLE */

#if (XIAOZHI_MANAGER_ENABLE != 1)
#pragma GCC diagnostic pop
#endif
