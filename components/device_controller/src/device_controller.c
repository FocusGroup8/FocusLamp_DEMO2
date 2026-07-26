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
static int s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
static arm_position_t s_arm_position = ARM_POSITION_HOME;
static bool s_speaker_playing = false;

/*---------------------------------------------------------------
 * Forward declarations for MCP tool callbacks
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_arm_come_here(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_arm_go_back(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_arm_dance(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_speaker_play_song(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_speaker_stop(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * Public API - Arm control (light control delegated to mipi_dsi_bridge)
 *-------------------------------------------------------------*/
#if (DEVICE_CONTROLLER_ENABLE == 1)

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
 * MCP tool callbacks (light tools removed — delegated to mipi_dsi_bridge)
 *-------------------------------------------------------------*/

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

    /* Light tools removed — delegated to mipi_dsi_bridge (self.mipi_dsi.led.*) */

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

    ESP_LOGI(TAG, "Device controller MCP tools registered (5 tools, light delegated to mipi_dsi_bridge)");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t device_controller_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    /* Rule e: default brightness based on simulated ambient light */
    s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
    s_arm_position = ARM_POSITION_HOME;
    s_speaker_playing = false;

    s_initialized = true;
    ESP_LOGI(TAG, "Device controller initialized (light control delegated to mipi_dsi_bridge)");
    return ESP_OK;
}

esp_err_t device_controller_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_volume = DEVICE_CONTROLLER_DEFAULT_VOLUME;
    s_arm_position = ARM_POSITION_HOME;
    s_speaker_playing = false;
    s_initialized = false;

    ESP_LOGI(TAG, "Device controller deinitialized");
    return ESP_OK;
}

#else /* DEVICE_CONTROLLER_ENABLE == 0 */

/* Stub implementations when component is disabled */

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
