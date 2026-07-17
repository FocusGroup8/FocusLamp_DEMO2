/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_EXAMPLE_ENABLE_CAMERA

#include "board_config.h"
#include "camera_controller.h"
#include "esp_log.h"

static const char *TAG = "CAMERA_TEST";

#define CAMERA_TEST_FRAME_COUNT 5

esp_err_t camera_test_run(camera_handles_t *handles)
{
    ESP_LOGI(TAG, "===== Camera Controller Test Start =====");

    if (!handles || !handles->is_initialized) {
        ESP_LOGE(TAG, "Camera not initialized, cannot run test");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

    /* Start streaming if not already streaming */
    if (!handles->is_streaming) {
        ESP_LOGI(TAG, "Starting camera stream for test...");
        ret = camera_controller_start(handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera start failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* Capture frames */
    ESP_LOGI(TAG, "Capturing %d frames...", CAMERA_TEST_FRAME_COUNT);
    for (int i = 0; i < CAMERA_TEST_FRAME_COUNT; i++) {
        ret = camera_capture_frame(handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Frame %d capture failed: %s", i, esp_err_to_name(ret));
            break;
        }
        ESP_LOGI(TAG, "  Frame %d: addr=%p, size=%zu bytes", i, handles->frame_buffer, handles->frame_buffer_size);
    }

    ESP_LOGI(TAG, "===== Camera Controller Test Complete =====");
    return ESP_OK;
}

#endif /* CONFIG_EXAMPLE_ENABLE_CAMERA */
