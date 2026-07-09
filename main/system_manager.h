/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file system_manager.h
 * @brief System manager for mutually exclusive audio/display operation
 *
 * This module manages system-level operation mode selection and resource
 * allocation, ensuring that audio and display subsystems operate exclusively.
 *
 * Operation modes (configured via Kconfig):
 * - AUDIO_ONLY: Runs audio system without LCD display
 * - DISPLAY_ONLY: Runs display system without audio hardware
 *
 * Runtime enforcement:
 * - System manager checks configuration and initializes only one subsystem
 * - Prevents resource conflicts (I2S vs MIPI DSI, memory allocation)
 * - Provides unified lifecycle management (init/start/stop/deinit)
 */

#include "audio_system.h"
#include "display_system.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief System operation modes (mutually exclusive)
 */
typedef enum {
    SYSTEM_MODE_AUDIO_ONLY,   /*!< Audio subsystem only (no LCD) */
    SYSTEM_MODE_DISPLAY_ONLY, /*!< Display subsystem only (no audio) */
    SYSTEM_MODE_UNDEFINED,    /*!< Mode not set (error state) */
} system_mode_t;

/**
 * @brief System manager state
 */
typedef enum {
    SYSTEM_STATE_UNINITIALIZED, /*!< System manager not initialized */
    SYSTEM_STATE_IDLE,          /*!< Subsystem initialized but not running */
    SYSTEM_STATE_RUNNING,       /*!< Subsystem actively running */
    SYSTEM_STATE_ERROR,         /*!< System error state */
} system_state_t;

/**
 * @brief System manager configuration
 */
typedef struct {
    system_mode_t mode;              /*!< Operation mode (audio-only or display-only) */
    audio_config_t audio_config;     /*!< Audio subsystem configuration (if AUDIO_ONLY) */
    display_config_t display_config; /*!< Display subsystem configuration (if DISPLAY_ONLY) */
} system_config_t;

/**
 * @brief Initialize system manager
 *
 * Reads system mode from Kconfig configuration and initializes
 * the appropriate subsystem (audio or display).
 *
 * - If AUDIO_ONLY mode: initializes audio_system
 * - If DISPLAY_ONLY mode: initializes display_system
 *
 * @param[out] handles  System handles (audio or display, depending on mode)
 * @return ESP_OK on success
 */
esp_err_t system_manager_init(system_config_t *config);

/**
 * @brief Start system operation
 *
 * Starts the active subsystem (audio or display) in its configured mode.
 *
 * @return ESP_OK on success
 */
esp_err_t system_manager_start(void);

/**
 * @brief Stop system operation
 *
 * Stops the active subsystem without deinitializing.
 * Can be restarted with system_manager_start().
 *
 * @return ESP_OK on success
 */
esp_err_t system_manager_stop(void);

/**
 * @brief Get current system mode
 *
 * @return Current operation mode (AUDIO_ONLY or DISPLAY_ONLY)
 */
system_mode_t system_manager_get_mode(void);

/**
 * @brief Get current system state
 *
 * @return Current state (UNINITIALIZED, IDLE, RUNNING, or ERROR)
 */
system_state_t system_manager_get_state(void);

/**
 * @brief Deinitialize system manager
 *
 * Deinitializes the active subsystem and releases all resources.
 */
void system_manager_deinit(void);

/**
 * @brief Check if audio system is active
 *
 * @return true if SYSTEM_MODE_AUDIO_ONLY and audio is initialized
 */
bool system_manager_is_audio_active(void);

/**
 * @brief Check if display system is active
 *
 * @return true if SYSTEM_MODE_DISPLAY_ONLY and display is initialized
 */
bool system_manager_is_display_active(void);

#ifdef __cplusplus
}
#endif