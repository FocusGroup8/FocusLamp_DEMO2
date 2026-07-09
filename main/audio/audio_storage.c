/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_storage.h"

#include "board_config.h"
#include "dirent.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "AUDIO_STORAGE";

esp_err_t audio_storage_init(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS storage...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = BOARD_AUDIO_MOUNT_POINT,
        .partition_label        = BOARD_AUDIO_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition with label '%s'", BOARD_AUDIO_PARTITION_LABEL);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "LittleFS already mounted at '%s'", BOARD_AUDIO_MOUNT_POINT);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    // Get partition information
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(BOARD_AUDIO_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted successfully: total=%zu KB, used=%zu KB, free=%zu KB", total / 1024,
                 used / 1024, (total - used) / 1024);
    } else {
        ESP_LOGW(TAG, "LittleFS mounted but failed to get info: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

void audio_storage_deinit(void)
{
    ESP_LOGI(TAG, "Unmounting LittleFS storage...");
    esp_vfs_littlefs_unregister(BOARD_AUDIO_PARTITION_LABEL);
    ESP_LOGI(TAG, "LittleFS storage unmounted");
}

esp_err_t audio_storage_list_files(int *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    // Try to list files in /storage/audio subdirectory first
    char audio_dir_path[128];
    snprintf(audio_dir_path, sizeof(audio_dir_path), "%s/audio", BOARD_AUDIO_MOUNT_POINT);

    DIR *dir = opendir(audio_dir_path);
    if (dir != NULL) {
        ESP_LOGI(TAG, "Listing files in '%s':", audio_dir_path);
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                ESP_LOGI(TAG, "  Found file: %s", entry->d_name);
                (*count)++;
            }
        }
        closedir(dir);
        ESP_LOGI(TAG, "Total files found in audio subdirectory: %d", *count);
        return ESP_OK;
    }

    // If audio subdirectory doesn't exist, try root mount point
    dir = opendir(BOARD_AUDIO_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory '%s'", BOARD_AUDIO_MOUNT_POINT);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Listing files in '%s':", BOARD_AUDIO_MOUNT_POINT);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            ESP_LOGI(TAG, "  Found file: %s", entry->d_name);
            (*count)++;
        } else if (entry->d_type == DT_DIR) {
            // Recursively check subdirectories
            char subdir_path[512];
            snprintf(subdir_path, sizeof(subdir_path), "%s/%s", BOARD_AUDIO_MOUNT_POINT, entry->d_name);
            DIR *subdir = opendir(subdir_path);
            if (subdir != NULL) {
                ESP_LOGI(TAG, "  Checking subdirectory: %s", entry->d_name);
                struct dirent *subentry;
                while ((subentry = readdir(subdir)) != NULL) {
                    if (subentry->d_type == DT_REG) {
                        ESP_LOGI(TAG, "    Found file: %s/%s", entry->d_name, subentry->d_name);
                        (*count)++;
                    }
                }
                closedir(subdir);
            }
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Total files found: %d", *count);
    return ESP_OK;
}

esp_err_t audio_storage_get_space(size_t *total_bytes, size_t *free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t used   = 0;
    esp_err_t ret = esp_littlefs_info(BOARD_AUDIO_PARTITION_LABEL, total_bytes, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get storage info: %s", esp_err_to_name(ret));
        return ret;
    }

    *free_bytes = *total_bytes - used;
    return ESP_OK;
}