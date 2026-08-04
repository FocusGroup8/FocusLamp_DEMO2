/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file system_manager.h
 * @brief System manager for mutually exclusive Top/Bottom board operation
 *
 * This module manages board type selection and resource allocation,
 * ensuring that Top and Bottom board subsystems operate exclusively.
 *
 * Board types (configured via Kconfig):
 * - BOTTOM_BOARD: Runs audio system (no LCD display)
 * - TOP_BOARD: Runs display/LED/camera system (no audio hardware)
 *
 * Runtime enforcement:
 * - System manager checks configuration and initializes selected subsystems
 * - Prevents resource conflicts (I2S vs MIPI DSI, memory allocation)
 * - Provides unified lifecycle management (init/start/stop/deinit)
 */

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio_system.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "display_system.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
#include "camera_controller.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board type modes (mutually exclusive)
 */
typedef enum {
    SYSTEM_MODE_BOTTOM_BOARD, /*!< Bottom board: audio subsystem */
    SYSTEM_MODE_TOP_BOARD,    /*!< Top board: display/LED/camera subsystems */
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
    system_mode_t mode; /*!< Board type (bottom-board or top-board) */
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    audio_config_t audio_config; /*!< Audio subsystem configuration */
#endif
#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    display_config_t display_config; /*!< Display subsystem configuration */
#endif
#if CONFIG_EXAMPLE_ENABLE_CAMERA
    camera_config_t camera_config; /*!< Camera subsystem configuration */
#endif
} system_config_t;

/**
 * @brief Initialize system manager
 *
 * Reads board type from Kconfig configuration and initializes
 * the appropriate subsystem(s).
 *
 * - If BOTTOM_BOARD mode: initializes audio_system (if enabled)
 * - If TOP_BOARD mode: initializes display_system (if enabled)
 *
 * @param[out] config  System configuration
 * @return ESP_OK on success
 */
esp_err_t system_manager_init(system_config_t *config);

/**
 * @brief Start system operation
 *
 * Starts the active subsystem in its configured mode.
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
 * @return Current operation mode (BOTTOM_BOARD or TOP_BOARD)
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
 * @return true if SYSTEM_MODE_BOTTOM_BOARD and audio is initialized
 */
bool system_manager_is_audio_active(void);

/**
 * @brief Check if display system is active
 *
 * @return true if SYSTEM_MODE_TOP_BOARD and display is initialized
 */
bool system_manager_is_display_active(void);

#if CONFIG_EXAMPLE_ENABLE_CAMERA
/**
 * @brief Get camera pipeline handles
 *
 * Returns pointer to the internally managed camera handles.
 * Only valid when system is in TOP_BOARD mode and camera is initialized.
 *
 * @return Pointer to camera handles, or NULL if not initialized
 */
camera_handles_t *system_manager_get_camera_handles(void);
#endif

#ifdef __cplusplus
}
#endif
