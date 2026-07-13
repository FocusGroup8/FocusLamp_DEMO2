/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Light location enumeration
 */
typedef enum {
    LIGHT_LOCATION_HEAD,    /*!< Head light */
    LIGHT_LOCATION_BASE,    /*!< Base light */
    LIGHT_LOCATION_ALL,     /*!< All lights */
} light_location_t;

/*---------------------------------------------------------------
 * Light control
 *-------------------------------------------------------------*/

/**
 * @brief  Turn on light at specified location
 *
 * If no lights are currently on and location is LIGHT_LOCATION_ALL,
 * only the head light is turned on (per requirement rule a).
 *
 * @param location  Which light(s) to turn on
 * @return ESP_OK on success
 */
esp_err_t device_light_on(light_location_t location);

/**
 * @brief  Turn off light at specified location
 *
 * When location is LIGHT_LOCATION_ALL, turns off ALL lights
 * regardless of how many are on (per requirement rule c).
 *
 * @param location  Which light(s) to turn off
 * @return ESP_OK on success
 */
esp_err_t device_light_off(light_location_t location);

/**
 * @brief  Adjust brightness by delta steps
 *
 * Each step is DEVICE_CONTROLLER_LIGHT_BRIGHTNESS_STEP.
 * If brightness is already at max/min, returns ESP_OK and logs a warning.
 *
 * @param delta  Brightness change (+/- steps)
 * @return ESP_OK on success
 */
esp_err_t device_light_adjust_brightness(int delta);

/**
 * @brief  Get current brightness
 *
 * @param[out] brightness  Current brightness (0-100)
 * @return ESP_OK on success
 */
esp_err_t device_light_get_brightness(int *brightness);

/**
 * @brief  Set brightness directly
 *
 * @param brightness  Target brightness (0-100)
 * @return ESP_OK on success
 */
esp_err_t device_light_set_brightness(int brightness);

/*---------------------------------------------------------------
 * Arm control
 *-------------------------------------------------------------*/

/**
 * @brief  Move arm toward user
 *
 * @return ESP_OK on success
 */
esp_err_t device_arm_come_here(void);

/**
 * @brief  Return arm to home position
 *
 * @return ESP_OK on success
 */
esp_err_t device_arm_go_back(void);

/**
 * @brief  Arm performs dance movement
 *
 * @return ESP_OK on success
 */
esp_err_t device_arm_dance(void);

/*---------------------------------------------------------------
 * Speaker control
 *-------------------------------------------------------------*/

/**
 * @brief  Play a song
 *
 * @param song_name  Name of the song to play
 * @return ESP_OK on success
 */
esp_err_t device_speaker_play_song(const char *song_name);

/**
 * @brief  Stop playback
 *
 * @return ESP_OK on success
 */
esp_err_t device_speaker_stop(void);

/**
 * @brief  Set speaker volume
 *
 * @param volume  Target volume (0-100)
 * @return ESP_OK on success
 */
esp_err_t device_speaker_set_volume(int volume);

/**
 * @brief  Get current speaker volume
 *
 * @param[out] volume  Current volume (0-100)
 * @return ESP_OK on success
 */
esp_err_t device_speaker_get_volume(int *volume);

/*---------------------------------------------------------------
 * MCP registration
 *-------------------------------------------------------------*/

/**
 * @brief  Register all device MCP tools
 *
 * Registers light, arm, and speaker MCP tools to the given MCP engine.
 *
 * @param mcp  MCP engine handle
 * @return ESP_OK on success
 */
esp_err_t device_controller_register_mcp_tools(esp_mcp_t *mcp);

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

/**
 * @brief  Initialize device controller
 *
 * Initializes simulated device state with default values.
 *
 * @return ESP_OK on success
 */
esp_err_t device_controller_init(void);

/**
 * @brief  Deinitialize device controller
 *
 * Resets all simulated device state.
 *
 * @return ESP_OK on success
 */
esp_err_t device_controller_deinit(void);

#ifdef __cplusplus
}
#endif
