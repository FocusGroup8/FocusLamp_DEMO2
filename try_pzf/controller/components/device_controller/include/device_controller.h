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

/*---------------------------------------------------------------
 * Light control — delegated to mipi_dsi_bridge (self.mipi_dsi.led.*)
 *-------------------------------------------------------------*/

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
 * Registers arm and speaker MCP tools to the given MCP engine.
 * Light control is delegated to mipi_dsi_bridge (self.mipi_dsi.led.*).
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
