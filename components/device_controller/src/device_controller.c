/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"

#include "device_controller.h"
#include "device_controller_config.h"

#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"

static const char *TAG = "DEVICE_CTRL";

/*---------------------------------------------------------------
 * Simulated device state
 *-------------------------------------------------------------*/
typedef enum {
    ARM_POSITION_HOME,       /*!< Arm at home position */
    ARM_POSITION_TOWARD_USER, /*!< Arm moved toward user */
    ARM_POSITION_DANCING,    /*!< Arm performing dance */
} arm_position_t;

static bool s_initialized = false;
static bool s_head_light_on = false;
static bool s_base_light_on = false;
static int s_brightness = 50;    /* Default brightness based on simulated ambient light */
static int s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
static arm_position_t s_arm_position = ARM_POSITION_HOME;
static bool s_speaker_playing = false;

/*---------------------------------------------------------------
 * Forward declarations for MCP tool callbacks
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_light_on(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_light_off(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_light_adjust_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_light_set_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_arm_come_here(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_arm_go_back(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_arm_dance(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_speaker_play_song(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_speaker_stop(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * Internal helpers
 *-------------------------------------------------------------*/
static light_location_t parse_location_string(const char *location_str)
{
    if (location_str && strcmp(location_str, "base") == 0) {
        return LIGHT_LOCATION_BASE;
    } else if (location_str && strcmp(location_str, "all") == 0) {
        return LIGHT_LOCATION_ALL;
    }
    /* Default to head */
    return LIGHT_LOCATION_HEAD;
}

/*---------------------------------------------------------------
 * Public API - Light control
 *-------------------------------------------------------------*/
#if (DEVICE_CONTROLLER_ENABLE == 1)

esp_err_t device_light_on(light_location_t location)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(location >= LIGHT_LOCATION_HEAD && location <= LIGHT_LOCATION_ALL,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid location");

    if (location == LIGHT_LOCATION_HEAD || location == LIGHT_LOCATION_ALL) {
        /* Rule a: when no lights are on and "all" is requested, turn on head only */
        if (location == LIGHT_LOCATION_ALL && !s_head_light_on && !s_base_light_on) {
            s_head_light_on = true;
            ESP_LOGI(TAG, "Light on: head (no lights were on, turning on head only per rule)");
        } else {
            s_head_light_on = true;
            ESP_LOGI(TAG, "Light on: head");
            if (location == LIGHT_LOCATION_ALL) {
                s_base_light_on = true;
                ESP_LOGI(TAG, "Light on: base");
            }
        }
    } else if (location == LIGHT_LOCATION_BASE) {
        s_base_light_on = true;
        ESP_LOGI(TAG, "Light on: base");
    }

    return ESP_OK;
}

esp_err_t device_light_off(light_location_t location)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(location >= LIGHT_LOCATION_HEAD && location <= LIGHT_LOCATION_ALL,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid location");

    /* Rule c: "light off" turns off ALL lights regardless of how many are on */
    if (location == LIGHT_LOCATION_ALL) {
        s_head_light_on = false;
        s_base_light_on = false;
        ESP_LOGI(TAG, "Light off: all lights");
    } else if (location == LIGHT_LOCATION_HEAD) {
        s_head_light_on = false;
        ESP_LOGI(TAG, "Light off: head");
    } else if (location == LIGHT_LOCATION_BASE) {
        s_base_light_on = false;
        ESP_LOGI(TAG, "Light off: base");
    }

    return ESP_OK;
}

esp_err_t device_light_adjust_brightness(int delta)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    int new_brightness = s_brightness + delta * DEVICE_CONTROLLER_LIGHT_BRIGHTNESS_STEP;

    /* Rule d: if brightness already at max/min, log warning */
    if (new_brightness > DEVICE_CONTROLLER_LIGHT_MAX_BRIGHTNESS) {
        ESP_LOGW(TAG, "Brightness already at max (%d), cannot increase", s_brightness);
        s_brightness = DEVICE_CONTROLLER_LIGHT_MAX_BRIGHTNESS;
        return ESP_OK;
    }
    if (new_brightness < DEVICE_CONTROLLER_LIGHT_MIN_BRIGHTNESS) {
        ESP_LOGW(TAG, "Brightness already at min (%d), cannot decrease", s_brightness);
        s_brightness = DEVICE_CONTROLLER_LIGHT_MIN_BRIGHTNESS;
        return ESP_OK;
    }

    s_brightness = new_brightness;
    ESP_LOGI(TAG, "Brightness adjusted to %d (delta=%d)", s_brightness, delta);
    return ESP_OK;
}

esp_err_t device_light_get_brightness(int *brightness)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(brightness, ESP_ERR_INVALID_ARG, TAG, "Invalid brightness pointer");

    *brightness = s_brightness;
    return ESP_OK;
}

esp_err_t device_light_set_brightness(int brightness)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(brightness >= DEVICE_CONTROLLER_LIGHT_MIN_BRIGHTNESS &&
                        brightness <= DEVICE_CONTROLLER_LIGHT_MAX_BRIGHTNESS,
                        ESP_ERR_INVALID_ARG, TAG, "Brightness out of range (0-100)");

    s_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to %d", s_brightness);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API - Arm control
 *-------------------------------------------------------------*/

esp_err_t device_arm_come_here(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    s_arm_position = ARM_POSITION_TOWARD_USER;
    ESP_LOGI(TAG, "Arm moving toward user");
    return ESP_OK;
}

esp_err_t device_arm_go_back(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    s_arm_position = ARM_POSITION_HOME;
    ESP_LOGI(TAG, "Arm returning to home position");
    return ESP_OK;
}

esp_err_t device_arm_dance(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    s_arm_position = ARM_POSITION_DANCING;
    ESP_LOGI(TAG, "Arm performing dance movement");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API - Speaker control
 *-------------------------------------------------------------*/

esp_err_t device_speaker_play_song(const char *song_name)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(song_name, ESP_ERR_INVALID_ARG, TAG, "Invalid song name");

    s_speaker_playing = true;
    ESP_LOGI(TAG, "Playing song: %s", song_name);
    return ESP_OK;
}

esp_err_t device_speaker_stop(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    s_speaker_playing = false;
    ESP_LOGI(TAG, "Speaker stopped");
    return ESP_OK;
}

esp_err_t device_speaker_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "Volume out of range (0-100)");

    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d", s_volume);
    return ESP_OK;
}

esp_err_t device_speaker_get_volume(int *volume)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(volume, ESP_ERR_INVALID_ARG, TAG, "Invalid volume pointer");

    *volume = s_volume;
    return ESP_OK;
}

/*---------------------------------------------------------------
 * MCP tool callbacks
 *-------------------------------------------------------------*/

static esp_mcp_value_t mcp_tool_light_on(const esp_mcp_property_list_t *properties)
{
    const char *location_str = esp_mcp_property_list_get_property_string(properties, "location");
    light_location_t location = parse_location_string(location_str);

    ESP_LOGI(TAG, "[MCP] self.light.on: location=\"%s\"", location_str ? location_str : "null");
    device_light_on(location);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_off(const esp_mcp_property_list_t *properties)
{
    const char *location_str = esp_mcp_property_list_get_property_string(properties, "location");
    light_location_t location = parse_location_string(location_str);

    ESP_LOGI(TAG, "[MCP] self.light.off: location=\"%s\"", location_str ? location_str : "null");
    device_light_off(location);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_adjust_brightness(const esp_mcp_property_list_t *properties)
{
    const char *direction = esp_mcp_property_list_get_property_string(properties, "direction");
    int delta = 0;

    if (direction && strcmp(direction, "up") == 0) {
        delta = 1;
    } else if (direction && strcmp(direction, "down") == 0) {
        delta = -1;
    } else {
        ESP_LOGW(TAG, "[MCP] self.light.adjust_brightness: invalid direction \"%s\"",
                 direction ? direction : "null");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "[MCP] self.light.adjust_brightness: direction=\"%s\"", direction);
    device_light_adjust_brightness(delta);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_light_set_brightness(const esp_mcp_property_list_t *properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");

    ESP_LOGI(TAG, "[MCP] self.light.set_brightness: brightness=%d", brightness);
    device_light_set_brightness(brightness);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_come_here(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.come_here");
    device_arm_come_here();

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_go_back(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.go_back");
    device_arm_go_back();

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_arm_dance(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.arm.dance");
    device_arm_dance();

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_speaker_play_song(const esp_mcp_property_list_t *properties)
{
    const char *song_name = esp_mcp_property_list_get_property_string(properties, "song_name");

    ESP_LOGI(TAG, "[MCP] self.speaker.play_song: song_name=\"%s\"", song_name ? song_name : "null");
    device_speaker_play_song(song_name);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t mcp_tool_speaker_stop(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] self.speaker.stop");
    device_speaker_stop();

    return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * MCP tool registration
 *-------------------------------------------------------------*/

esp_err_t device_controller_register_mcp_tools(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP engine");

    /* self.light.on */
    esp_mcp_tool_t *light_on_tool = esp_mcp_tool_create(
        "self.light.on",
        "开灯，支持指定位置 (head/base/all)",
        mcp_tool_light_on
    );
    if (!light_on_tool) {
        ESP_LOGE(TAG, "Failed to create self.light.on tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *on_loc_prop = esp_mcp_property_create("location", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(light_on_tool, on_loc_prop);
    esp_mcp_add_tool(mcp, light_on_tool);

    /* self.light.off */
    esp_mcp_tool_t *light_off_tool = esp_mcp_tool_create(
        "self.light.off",
        "关灯，支持指定位置 (head/base/all)",
        mcp_tool_light_off
    );
    if (!light_off_tool) {
        ESP_LOGE(TAG, "Failed to create self.light.off tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *off_loc_prop = esp_mcp_property_create("location", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(light_off_tool, off_loc_prop);
    esp_mcp_add_tool(mcp, light_off_tool);

    /* self.light.adjust_brightness */
    esp_mcp_tool_t *adj_bright_tool = esp_mcp_tool_create(
        "self.light.adjust_brightness",
        "调节灯光亮度 (up/down)",
        mcp_tool_light_adjust_brightness
    );
    if (!adj_bright_tool) {
        ESP_LOGE(TAG, "Failed to create self.light.adjust_brightness tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *dir_prop = esp_mcp_property_create("direction", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(adj_bright_tool, dir_prop);
    esp_mcp_add_tool(mcp, adj_bright_tool);

    /* self.light.set_brightness */
    esp_mcp_tool_t *set_bright_tool = esp_mcp_tool_create(
        "self.light.set_brightness",
        "直接设置灯光亮度 (0-100)",
        mcp_tool_light_set_brightness
    );
    if (!set_bright_tool) {
        ESP_LOGE(TAG, "Failed to create self.light.set_brightness tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *bright_prop = esp_mcp_property_create_with_range("brightness", 0, 100);
    esp_mcp_tool_add_property(set_bright_tool, bright_prop);
    esp_mcp_add_tool(mcp, set_bright_tool);

    /* self.arm.come_here */
    esp_mcp_tool_t *arm_come_tool = esp_mcp_tool_create(
        "self.arm.come_here",
        "机械臂向用户移动",
        mcp_tool_arm_come_here
    );
    if (!arm_come_tool) {
        ESP_LOGE(TAG, "Failed to create self.arm.come_here tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, arm_come_tool);

    /* self.arm.go_back */
    esp_mcp_tool_t *arm_back_tool = esp_mcp_tool_create(
        "self.arm.go_back",
        "机械臂回到初始位置",
        mcp_tool_arm_go_back
    );
    if (!arm_back_tool) {
        ESP_LOGE(TAG, "Failed to create self.arm.go_back tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, arm_back_tool);

    /* self.arm.dance */
    esp_mcp_tool_t *arm_dance_tool = esp_mcp_tool_create(
        "self.arm.dance",
        "机械臂跳舞",
        mcp_tool_arm_dance
    );
    if (!arm_dance_tool) {
        ESP_LOGE(TAG, "Failed to create self.arm.dance tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, arm_dance_tool);

    /* self.speaker.play_song */
    esp_mcp_tool_t *play_song_tool = esp_mcp_tool_create(
        "self.speaker.play_song",
        "播放指定歌曲",
        mcp_tool_speaker_play_song
    );
    if (!play_song_tool) {
        ESP_LOGE(TAG, "Failed to create self.speaker.play_song tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *song_prop = esp_mcp_property_create("song_name", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(play_song_tool, song_prop);
    esp_mcp_add_tool(mcp, play_song_tool);

    /* self.speaker.stop */
    esp_mcp_tool_t *speaker_stop_tool = esp_mcp_tool_create(
        "self.speaker.stop",
        "停止播放",
        mcp_tool_speaker_stop
    );
    if (!speaker_stop_tool) {
        ESP_LOGE(TAG, "Failed to create self.speaker.stop tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, speaker_stop_tool);

    ESP_LOGI(TAG, "Device controller MCP tools registered (9 tools)");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t device_controller_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    /* Rule e: default brightness based on simulated ambient light */
    s_head_light_on = false;
    s_base_light_on = false;
    s_brightness = 50;
    s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
    s_arm_position = ARM_POSITION_HOME;
    s_speaker_playing = false;

    s_initialized = true;
    ESP_LOGI(TAG, "Device controller initialized (simulated mode)");
    return ESP_OK;
}

esp_err_t device_controller_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_head_light_on = false;
    s_base_light_on = false;
    s_brightness = 50;
    s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
    s_arm_position = ARM_POSITION_HOME;
    s_speaker_playing = false;
    s_initialized = false;

    ESP_LOGI(TAG, "Device controller deinitialized");
    return ESP_OK;
}

#else /* DEVICE_CONTROLLER_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t device_light_on(light_location_t location) { (void)location; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_light_off(light_location_t location) { (void)location; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_light_adjust_brightness(int delta) { (void)delta; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_light_get_brightness(int *brightness) { (void)brightness; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_light_set_brightness(int brightness) { (void)brightness; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_arm_come_here(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_arm_go_back(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_arm_dance(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_speaker_play_song(const char *song_name) { (void)song_name; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_speaker_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_speaker_set_volume(int volume) { (void)volume; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_speaker_get_volume(int *volume) { (void)volume; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_controller_register_mcp_tools(esp_mcp_t *mcp) { (void)mcp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_controller_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t device_controller_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* DEVICE_CONTROLLER_ENABLE */
