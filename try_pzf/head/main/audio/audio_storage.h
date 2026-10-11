/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Initialize and mount LittleFS partition
 *
 * Mounts the LittleFS partition specified by BOARD_AUDIO_PARTITION_LABEL
 * to BOARD_AUDIO_MOUNT_POINT. Formats the partition if mount fails.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_storage_init(void);

/**
 * @brief Unmount LittleFS partition
 */
void audio_storage_deinit(void);

/**
 * @brief List all audio files in storage
 *
 * @param[out] count  Number of files found
 * @return ESP_OK on success
 */
esp_err_t audio_storage_list_files(int *count);

/**
 * @brief Get available storage space
 *
 * @param[out] total_bytes  Total space in bytes
 * @param[out] free_bytes   Free space in bytes
 * @return ESP_OK on success
 */
esp_err_t audio_storage_get_space(size_t *total_bytes, size_t *free_bytes);