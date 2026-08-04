/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2s_driver.h"

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "I2S_DRIVER";

esp_err_t i2s_audio_init(i2s_audio_handles_t *handles)
{
    if (handles == NULL) {
        ESP_LOGE(TAG, "Invalid handles pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Step 1: Create channel pair (full-duplex)
    // Matches radar_test configuration exactly
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, BOARD_I2S_ROLE);
    chan_cfg.dma_desc_num      = 16;  // radar_test value
    chan_cfg.dma_frame_num     = 960; // radar_test value
    chan_cfg.auto_clear        = true;

    ESP_LOGI(TAG, "Creating I2S channels: port=%d, dma_desc=%d, dma_frame=%d", BOARD_I2S_PORT, chan_cfg.dma_desc_num,
             chan_cfg.dma_frame_num);

    esp_err_t ret = i2s_new_channel(&chan_cfg, &handles->tx_handle, &handles->rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel pair: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S channel pair created (TX=%p, RX=%p)", handles->tx_handle, handles->rx_handle);

    // Step 2: Configure TX and RX with the SAME std_cfg (like radar_test)
    // KEY: radar_test uses the same i2s_std_config_t for both TX and RX,
    // with BOTH dout and din specified in the same config.
    // This is how ESP-IDF full-duplex is designed to work.
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = BOARD_I2S_BCLK_GPIO,
                .ws   = BOARD_I2S_WS_GPIO,
                .dout = BOARD_I2S_DOUT_GPIO,
                .din  = BOARD_I2S_DIN_GPIO,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_LOGI(TAG, "Config: PHILIPS, MONO, LEFT, %dHz, 32-bit", BOARD_I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "GPIO: BCLK=%d, WS=%d, DOUT=%d, DIN=%d", BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO, BOARD_I2S_DOUT_GPIO,
             BOARD_I2S_DIN_GPIO);

    ret = i2s_channel_init_std_mode(handles->tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(handles->tx_handle);
        i2s_del_channel(handles->rx_handle);
        handles->tx_handle = NULL;
        handles->rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "TX channel initialized");

    ret = i2s_channel_init_std_mode(handles->rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(handles->tx_handle);
        i2s_del_channel(handles->rx_handle);
        handles->tx_handle = NULL;
        handles->rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "RX channel initialized");

    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

esp_err_t i2s_audio_enable(const i2s_audio_handles_t *handles)
{
    if (handles == NULL || handles->tx_handle == NULL || handles->rx_handle == NULL) {
        ESP_LOGE(TAG, "Invalid handles");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // Preload TX DMA buffers before enabling (per ESP-IDF example)
    // This ensures valid data is transmitted immediately after enable
    int32_t preload_buf[64] = {0};
    size_t preload_bytes    = sizeof(preload_buf);
    int preload_count       = 0;
    while (preload_bytes == sizeof(preload_buf)) {
        ret = i2s_channel_preload_data(handles->tx_handle, preload_buf, sizeof(preload_buf), &preload_bytes);
        if (ret != ESP_OK)
            break;
        preload_count++;
    }
    ESP_LOGI(TAG, "Preloaded %d TX DMA buffers", preload_count);

    // Enable TX first (like radar_test)
    ret = i2s_channel_enable(handles->tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable RX
    ret = i2s_channel_enable(handles->rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        i2s_channel_disable(handles->tx_handle);
        return ret;
    }

    // Delay after enabling (like radar_test: 100ms)
    vTaskDelay(pdMS_TO_TICKS(100));

    // Test read to verify RX is working (like radar_test)
    int32_t test_buffer[64];
    size_t test_bytes_read;
    ESP_LOGI(TAG, "Testing RX read after enable...");
    for (int i = 0; i < 3; i++) {
        ret = i2s_channel_read(handles->rx_handle, test_buffer, sizeof(test_buffer), &test_bytes_read,
                               pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "  Test read %d: ret=%s, bytes=%u", i + 1, esp_err_to_name(ret), (unsigned)test_bytes_read);
        if (test_bytes_read > 0) {
            ESP_LOGI(TAG, "    First 4 values: 0x%08X 0x%08X 0x%08X 0x%08X", (uint32_t)test_buffer[0],
                     (uint32_t)test_buffer[1], (uint32_t)test_buffer[2], (uint32_t)test_buffer[3]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "I2S channels enabled");
    return ESP_OK;
}

esp_err_t i2s_audio_disable(const i2s_audio_handles_t *handles)
{
    if (handles == NULL || handles->tx_handle == NULL || handles->rx_handle == NULL) {
        ESP_LOGE(TAG, "Invalid handles");
        return ESP_ERR_INVALID_ARG;
    }

    i2s_channel_disable(handles->tx_handle);
    i2s_channel_disable(handles->rx_handle);

    ESP_LOGI(TAG, "I2S channels disabled");
    return ESP_OK;
}

esp_err_t i2s_audio_write(const i2s_audio_handles_t *handles, const void *data, size_t len, size_t *bytes_written,
                          uint32_t timeout_ms)
{
    if (handles == NULL || handles->tx_handle == NULL || data == NULL || bytes_written == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(handles->tx_handle, data, len, bytes_written, ticks_to_wait);
}

esp_err_t i2s_audio_read(const i2s_audio_handles_t *handles, void *data, size_t len, size_t *bytes_read,
                         uint32_t timeout_ms)
{
    if (handles == NULL || handles->rx_handle == NULL || data == NULL || bytes_read == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_read(handles->rx_handle, data, len, bytes_read, ticks_to_wait);
}

void i2s_audio_deinit(i2s_audio_handles_t *handles)
{
    if (handles == NULL) {
        return;
    }

    // Note: i2s_del_channel will automatically disable the channel if enabled,
    // so we don't need to call i2s_channel_disable separately before deleting.
    // This avoids the "channel has not been enabled yet" error when
    // i2s_audio_disable() was already called before deinit.

    if (handles->tx_handle) {
        i2s_del_channel(handles->tx_handle);
        handles->tx_handle = NULL;
    }

    if (handles->rx_handle) {
        i2s_del_channel(handles->rx_handle);
        handles->rx_handle = NULL;
    }

    ESP_LOGI(TAG, "I2S deinitialized");
}
