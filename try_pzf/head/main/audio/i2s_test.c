/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2s_test.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "I2S_TEST";

// Test configuration
#define TEST_SAMPLE_RATE 16000
#define TEST_TONE_FREQ 1000 // 1kHz sine wave
#define TEST_FRAMES 512     // Number of frames per buffer
#define TEST_AMPLITUDE 0.5f // 50% amplitude

// In MONO mode, each frame = 1 sample (left channel only)
// Buffer holds TEST_FRAMES samples = TEST_FRAMES int32_t values
#define TEST_BUFFER_SAMPLES TEST_FRAMES // Total int32_t samples (MONO: 1 sample per frame)
#define TEST_BUFFER_BYTES (TEST_BUFFER_SAMPLES * sizeof(int32_t))

/**
 * @brief Generate 32-bit signed sine wave samples in MONO format
 * In MONO+LEFT mode, each frame = 1 sample (left channel).
 * Buffer layout: [S0, S1, S2, ...]
 */
static void generate_sine_wave_mono(int32_t *buffer, size_t frames, float frequency, float amplitude)
{
    for (size_t i = 0; i < frames; i++) {
        float t     = (float)i / TEST_SAMPLE_RATE;
        float value = amplitude * sinf(2.0f * M_PI * frequency * t);
        // Generate 32-bit sample with audio data in upper 16 bits
        buffer[i] = (int32_t)(value * 32767.0f) << 16;
    }
    // Debug: print first 8 samples
    ESP_LOGI(TAG, "Sine wave first 8 samples (freq=%.0fHz, amp=%.2f, MONO):", frequency, amplitude);
    for (int i = 0; i < 8 && i < (int)frames; i++) {
        ESP_LOGI(TAG, "  sample[%d]: 0x%08X(%d)", i, (uint32_t)buffer[i], (int16_t)(buffer[i] >> 16));
    }
}

/**
 * @brief Calculate RMS amplitude from MONO buffer
 * In MONO mode, each sample is a single channel value.
 */
static float calculate_rms_mono(const int32_t *samples, size_t total_samples)
{
    if (total_samples < 1)
        return 0.0f;

    float sum = 0.0f;
    for (size_t i = 0; i < total_samples; i++) {
        int16_t val16    = (int16_t)(samples[i] >> 16);
        float normalized = (float)val16 / 32767.0f;
        sum += normalized * normalized;
    }
    return sqrtf(sum / total_samples);
}

esp_err_t i2s_test_channel_lifecycle(void)
{
    ESP_LOGI(TAG, "=== Test: Channel Lifecycle ===");

    i2s_audio_handles_t handles = {0};

    // Step 1: Create channels
    ESP_LOGI(TAG, "Creating I2S channels...");
    esp_err_t ret = i2s_audio_init(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel creation failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✓ Channels created (TX=%p, RX=%p)", handles.tx_handle, handles.rx_handle);

    // Step 2: Enable channels
    ESP_LOGI(TAG, "Enabling I2S channels...");
    ret = i2s_audio_enable(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel enable failed: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&handles);
        return ret;
    }
    ESP_LOGI(TAG, "✓ Channels enabled");

    // Step 3: Disable channels
    ESP_LOGI(TAG, "Disabling I2S channels...");
    ret = i2s_audio_disable(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel disable failed: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&handles);
        return ret;
    }
    ESP_LOGI(TAG, "✓ Channels disabled");

    // Step 4: Deinitialize
    ESP_LOGI(TAG, "Deinitializing I2S...");
    i2s_audio_deinit(&handles);
    ESP_LOGI(TAG, "✓ I2S deinitialized");

    ESP_LOGI(TAG, "=== Channel Lifecycle Test PASSED ===");
    return ESP_OK;
}

esp_err_t i2s_test_tx_sine_wave(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "=== Test: TX Sine Wave Output ===");

    i2s_audio_handles_t handles = {0};

    // Initialize and enable
    esp_err_t ret = i2s_audio_init(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_audio_enable(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable failed: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&handles);
        return ret;
    }

    // Generate sine wave buffer (MONO, heap-allocated to avoid stack overflow)
    int32_t *sine_buffer = malloc(TEST_BUFFER_SAMPLES * sizeof(int32_t));
    if (sine_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sine buffer");
        i2s_audio_disable(&handles);
        i2s_audio_deinit(&handles);
        return ESP_ERR_NO_MEM;
    }
    generate_sine_wave_mono(sine_buffer, TEST_FRAMES, TEST_TONE_FREQ, TEST_AMPLITUDE);

    ESP_LOGI(TAG, "Outputting %dHz sine wave for %lu ms...", TEST_TONE_FREQ, duration_ms);
    ESP_LOGI(TAG, "Check MAX98357A amplifier for audible tone");

    // Write sine wave repeatedly for specified duration
    uint32_t start_time = xTaskGetTickCount();
    size_t total_bytes  = 0;
    int write_count     = 0;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(duration_ms)) {
        size_t bytes_written = 0;
        ret                  = i2s_audio_write(&handles, sine_buffer, TEST_BUFFER_BYTES, &bytes_written, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(ret));
            break;
        }
        total_bytes += bytes_written;
        write_count++;
        // Log first few writes for debugging
        if (write_count <= 3) {
            ESP_LOGI(TAG, "TX write #%d: requested=%u, written=%u", write_count, (unsigned)TEST_BUFFER_BYTES,
                     (unsigned)bytes_written);
        }
    }

    ESP_LOGI(TAG, "✓ Wrote %lu bytes to TX channel (%d writes)", total_bytes, write_count);

    // Cleanup
    free(sine_buffer);
    i2s_audio_disable(&handles);
    i2s_audio_deinit(&handles);

    ESP_LOGI(TAG, "=== TX Sine Wave Test PASSED ===");
    return ESP_OK;
}

esp_err_t i2s_test_rx_microphone(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "=== Test: RX Microphone Input ===");

    i2s_audio_handles_t handles = {0};

    // Initialize and enable
    esp_err_t ret = i2s_audio_init(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_audio_enable(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable failed: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&handles);
        return ret;
    }

    ESP_LOGI(TAG, "Reading from INMP441 microphone for %lu ms...", duration_ms);
    ESP_LOGI(TAG, "Speak into microphone to verify data capture");

    // Heap-allocate RX buffer (MONO: TEST_FRAMES int32_t)
    int32_t *rx_buffer = malloc(TEST_BUFFER_SAMPLES * sizeof(int32_t));
    if (rx_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        i2s_audio_disable(&handles);
        i2s_audio_deinit(&handles);
        return ESP_ERR_NO_MEM;
    }
    uint32_t start_time    = xTaskGetTickCount();
    size_t total_frames    = 0;
    float max_rms          = 0.0f;
    int read_count         = 0;
    bool first_read_logged = false;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(duration_ms)) {
        size_t bytes_read = 0;
        ret               = i2s_audio_read(&handles, rx_buffer, TEST_BUFFER_BYTES, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
            break;
        }

        if (bytes_read > 0) {
            size_t samples_read = bytes_read / sizeof(int32_t);
            total_frames += samples_read; // MONO: 1 sample per frame
            read_count++;

            // Debug: print first read's raw sample values
            if (!first_read_logged) {
                ESP_LOGI(TAG, "First RX read: %u bytes, %u samples (MONO)", (unsigned)bytes_read,
                         (unsigned)samples_read);
                ESP_LOGI(TAG, "First 8 samples (MONO):");
                for (int i = 0; i < 8 && i < (int)samples_read; i++) {
                    int16_t val16 = (int16_t)(rx_buffer[i] >> 16);
                    ESP_LOGI(TAG, "  sample[%d]: 0x%08X(%d)", i, (uint32_t)rx_buffer[i], val16);
                }
                first_read_logged = true;
            }

            // RMS calculation (MONO: all samples are left channel)
            float rms = calculate_rms_mono(rx_buffer, samples_read);
            if (rms > max_rms) {
                max_rms = rms;
            }

            // Print periodic updates
            if (read_count % 50 == 0) {
                ESP_LOGI(TAG, "RX progress: frames=%lu, current_RMS=%.4f, max_RMS=%.4f", total_frames, rms, max_rms);
            }
        }
    }

    ESP_LOGI(TAG, "✓ Read %lu frames, Max RMS amplitude: %.4f", total_frames, max_rms);

    if (max_rms < 0.01f) {
        ESP_LOGW(TAG, "⚠ Very low audio level detected - check microphone connection");
    } else {
        ESP_LOGI(TAG, "✓ Microphone appears functional");
    }

    // Cleanup
    free(rx_buffer);
    i2s_audio_disable(&handles);
    i2s_audio_deinit(&handles);

    ESP_LOGI(TAG, "=== RX Microphone Test PASSED ===");
    return ESP_OK;
}

esp_err_t i2s_test_full_duplex(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "=== Test: Full-Duplex Simultaneous TX/RX ===");

    i2s_audio_handles_t handles = {0};

    // Initialize and enable
    esp_err_t ret = i2s_audio_init(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_audio_enable(&handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable failed: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&handles);
        return ret;
    }

    // Prepare TX sine wave (MONO, heap-allocated to avoid stack overflow)
    int32_t *tx_buffer = malloc(TEST_BUFFER_SAMPLES * sizeof(int32_t));
    int32_t *rx_buffer = malloc(TEST_BUFFER_SAMPLES * sizeof(int32_t));
    if (tx_buffer == NULL || rx_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TX/RX buffers");
        free(tx_buffer);
        free(rx_buffer);
        i2s_audio_disable(&handles);
        i2s_audio_deinit(&handles);
        return ESP_ERR_NO_MEM;
    }
    generate_sine_wave_mono(tx_buffer, TEST_FRAMES, TEST_TONE_FREQ, TEST_AMPLITUDE);

    ESP_LOGI(TAG, "Running TX output + RX capture for %lu ms...", duration_ms);
    ESP_LOGI(TAG, "Should hear tone AND see microphone data simultaneously");

    uint32_t start_time  = xTaskGetTickCount();
    size_t tx_bytes      = 0;
    size_t rx_bytes      = 0;
    float max_rms        = 0.0f;
    int loop_count       = 0;
    bool first_rx_logged = false;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(duration_ms)) {
        // Write TX (sine wave, MONO)
        size_t bytes_written = 0;
        ret                  = i2s_audio_write(&handles, tx_buffer, TEST_BUFFER_BYTES, &bytes_written, 1000);
        if (ret == ESP_OK) {
            tx_bytes += bytes_written;
        }

        // Read RX (microphone, MONO)
        size_t bytes_read = 0;
        ret               = i2s_audio_read(&handles, rx_buffer, TEST_BUFFER_BYTES, &bytes_read, 1000);
        if (ret == ESP_OK && bytes_read > 0) {
            rx_bytes += bytes_read;
            size_t samples = bytes_read / sizeof(int32_t);
            float rms      = calculate_rms_mono(rx_buffer, samples);
            if (rms > max_rms)
                max_rms = rms;

            // Debug: log first RX data
            if (!first_rx_logged) {
                ESP_LOGI(TAG,
                         "Full-duplex first RX: %u bytes, %u samples (MONO), first 4 samples:", (unsigned)bytes_read,
                         (unsigned)samples);
                for (int i = 0; i < 4 && i < (int)samples; i++) {
                    int16_t val16 = (int16_t)(rx_buffer[i] >> 16);
                    ESP_LOGI(TAG, "  sample[%d]: 0x%08X(%d)", i, (uint32_t)rx_buffer[i], val16);
                }
                first_rx_logged = true;
            }
        }

        loop_count++;
        if (loop_count % 50 == 0) {
            ESP_LOGI(TAG, "Full-duplex progress: TX=%lu bytes, RX=%lu bytes, max_RMS=%.4f", tx_bytes, rx_bytes,
                     max_rms);
        }
    }

    ESP_LOGI(TAG, "✓ TX wrote %lu bytes, RX read %lu bytes", tx_bytes, rx_bytes);
    ESP_LOGI(TAG, "✓ Max RX RMS: %.4f", max_rms);

    if (tx_bytes > 0 && rx_bytes > 0) {
        ESP_LOGI(TAG, "✓ Full-duplex operation verified - both channels working");
    } else {
        ESP_LOGW(TAG, "⚠ One or both channels failed during full-duplex test");
    }

    // Cleanup
    free(tx_buffer);
    free(rx_buffer);
    i2s_audio_disable(&handles);
    i2s_audio_deinit(&handles);

    ESP_LOGI(TAG, "=== Full-Duplex Test PASSED ===");
    return ESP_OK;
}

esp_err_t i2s_test_loopback_diagnostic(void)
{
    ESP_LOGI(TAG, "=== Diagnostic: I2S Internal Loopback ===");
    ESP_LOGI(TAG, "This test binds DIN=DOUT for internal loopback (per ESP-IDF example).");
    ESP_LOGI(TAG, "If loopback data matches TX data -> I2S peripheral is working -> hardware issue");
    ESP_LOGI(TAG, "If loopback data does NOT match -> software/driver issue");

    // Create standalone channel pair with DIN = DOUT for loopback
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num      = BOARD_AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num     = BOARD_AUDIO_DMA_BUF_LEN;
    chan_cfg.auto_clear        = false; // Disable auto_clear to see real data

    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t ret               = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: Failed to create channels: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure with PHILIPS format + MONO + LEFT (matches radar_test working config)
    // KEY: DIN = DOUT for internal loopback (as documented in ESP-IDF I2S docs)
    i2s_std_config_t loopback_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BOARD_I2S_DATA_BIT_WIDTH, BOARD_I2S_SLOT_MODE),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = BOARD_I2S_BCLK_GPIO,
                .ws   = BOARD_I2S_WS_GPIO,
                .dout = BOARD_I2S_DOUT_GPIO,
                .din  = BOARD_I2S_DOUT_GPIO, // LOOPBACK: DIN = DOUT (internal)
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
            },
    };
    loopback_cfg.clk_cfg.mclk_multiple = BOARD_I2S_MCLK_MULTIPLE;
    loopback_cfg.slot_cfg.slot_mask    = BOARD_I2S_SLOT_MASK;

    ret = i2s_channel_init_std_mode(tx_handle, &loopback_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: Failed to init TX: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        i2s_del_channel(rx_handle);
        return ret;
    }

    // RX uses same config (shares BCLK/WS, DIN=DOUT)
    ret = i2s_channel_init_std_mode(rx_handle, &loopback_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: Failed to init RX: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        i2s_del_channel(rx_handle);
        return ret;
    }

// Generate test pattern BEFORE preload and enable
#define LB_SAMPLES 512
    int32_t *tx_buf = malloc(LB_SAMPLES * sizeof(int32_t));
    int32_t *rx_buf = malloc(LB_SAMPLES * sizeof(int32_t));
    if (tx_buf == NULL || rx_buf == NULL) {
        ESP_LOGE(TAG, "Loopback: Failed to allocate buffers");
        free(tx_buf);
        free(rx_buf);
        i2s_del_channel(tx_handle);
        i2s_del_channel(rx_handle);
        return ESP_ERR_NO_MEM;
    }

    // Fill with known MONO pattern: alternating positive/negative
    for (int i = 0; i < LB_SAMPLES; i++) {
        tx_buf[i] = (i % 2 == 0) ? 0x40000000 : 0xC0000000;
    }

    // Preload TX DMA buffers with the test pattern
    size_t preload_bytes;
    int preload_count = 0;
    do {
        preload_bytes = 0;
        ret           = i2s_channel_preload_data(tx_handle, tx_buf, LB_SAMPLES * sizeof(int32_t), &preload_bytes);
        if (ret == ESP_OK && preload_bytes > 0)
            preload_count++;
    } while (ret == ESP_OK && preload_bytes > 0 && preload_count < 20);
    ESP_LOGI(TAG, "Loopback: Preloaded %d TX buffers", preload_count);

    // Enable TX first, then RX
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: Failed to enable TX: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: Failed to enable RX: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG, "Loopback: Sending known MONO pattern (0x40000000/0xC0000000 alternating)...");

    // Write more data to ensure DMA pipeline is full
    size_t bytes_written = 0;
    ret                  = i2s_channel_write(tx_handle, tx_buf, LB_SAMPLES * sizeof(int32_t), &bytes_written, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback: TX write failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Loopback: TX wrote %u bytes", (unsigned)bytes_written);

    // Delay for data to propagate through DMA
    vTaskDelay(pdMS_TO_TICKS(100));

    // Read RX - read multiple times to drain the DMA buffers
    size_t total_bytes_read = 0;
    int read_attempts       = 0;
    while (total_bytes_read < LB_SAMPLES * sizeof(int32_t) && read_attempts < 5) {
        size_t bytes_read = 0;
        size_t to_read    = LB_SAMPLES * sizeof(int32_t) - total_bytes_read;
        ret = i2s_channel_read(rx_handle, (uint8_t *)rx_buf + total_bytes_read, to_read, &bytes_read, 1000);
        if (ret == ESP_OK) {
            total_bytes_read += bytes_read;
        }
        read_attempts++;
    }
    ESP_LOGI(TAG, "Loopback: RX read %u bytes in %d attempts", (unsigned)total_bytes_read, read_attempts);

    // Compare samples
    size_t rx_samples = total_bytes_read / sizeof(int32_t);
    ESP_LOGI(TAG, "Loopback: First 8 samples comparison (TX vs RX):");
    for (int i = 0; i < 8 && i < (int)rx_samples; i++) {
        int32_t tx_val = (i < LB_SAMPLES) ? tx_buf[i] : 0;
        int32_t rx_val = rx_buf[i];
        ESP_LOGI(TAG, "  sample[%d]: TX=0x%08X | RX=0x%08X", i, (uint32_t)tx_val, (uint32_t)rx_val);
    }

    // Count matches (allow for DMA alignment offset: search for pattern anywhere)
    int match_count    = 0;
    int total_compared = 0;
    for (size_t i = 0; i < rx_samples && i < (size_t)LB_SAMPLES; i++) {
        if (rx_buf[i] == 0x40000000 || rx_buf[i] == 0xC0000000) {
            match_count++;
        }
        total_compared++;
    }

    float match_rate = (total_compared > 0) ? (float)match_count / total_compared * 100.0f : 0.0f;
    ESP_LOGI(TAG, "Loopback: Pattern match rate: %.1f%% (%d/%d)", match_rate, match_count, total_compared);

    if (match_rate > 80.0f) {
        ESP_LOGI(TAG, "Loopback PASSED - I2S peripheral is outputting correct data");
        ESP_LOGI(
            TAG,
            "  -> Speaker issue is likely HARDWARE (check DOUT->MAX98357A DIN connection, power, SD pin, speaker)");
    } else if (match_rate > 30.0f) {
        ESP_LOGW(TAG, "Loopback PARTIAL - data detected but misaligned or corrupted");
        ESP_LOGW(TAG, "  -> Possible timing issue or DMA alignment problem");
    } else {
        ESP_LOGE(TAG, "Loopback FAILED - RX data does not match TX data");
        ESP_LOGE(TAG, "  -> Software/driver issue: I2S peripheral may not be outputting data correctly");
    }

cleanup:
    free(tx_buf);
    free(rx_buf);
    i2s_channel_disable(rx_handle);
    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    i2s_del_channel(rx_handle);

    ESP_LOGI(TAG, "=== Loopback Diagnostic Complete ===");
    return ESP_OK;
}

/**
 * @brief GPIO test: verify DOUT pin can toggle
 * This rules out GPIO hardware issues before testing I2S.
 */
static esp_err_t i2s_test_gpio_toggle(void)
{
    ESP_LOGI(TAG, "=== Test: GPIO Toggle on DOUT ===");

    gpio_num_t pins[]   = {BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO, BOARD_I2S_DOUT_GPIO};
    const char *names[] = {"BCLK", "WS", "DOUT"};

    for (int p = 0; p < 3; p++) {
        gpio_num_t pin = pins[p];
        ESP_LOGI(TAG, "Testing GPIO %d (%s)...", pin, names[p]);

        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to config GPIO %d: %s", pin, esp_err_to_name(ret));
            continue;
        }

        // Toggle 10 times at ~100Hz
        for (int i = 0; i < 10; i++) {
            gpio_set_level(pin, 1);
            ets_delay_us(5);
            gpio_set_level(pin, 0);
            ets_delay_us(5);
        }
        ESP_LOGI(TAG, "  GPIO %d toggled OK", pin);

        // Reset pin to default state
        gpio_reset_pin(pin);
    }

    ESP_LOGI(TAG, "=== GPIO Toggle Test PASSED ===");
    return ESP_OK;
}

esp_err_t i2s_test_run_all(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "I2S Full-Duplex Test Suite");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret;

    // Test 0: GPIO toggle (verify pins are functional)
    ret = i2s_test_gpio_toggle();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO toggle test FAILED - pin hardware issue");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    // Test 1: Channel lifecycle
    ret = i2s_test_channel_lifecycle();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test suite FAILED at lifecycle test");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Test 2: TX sine wave (2 seconds)
    ret = i2s_test_tx_sine_wave(2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test suite FAILED at TX test");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Test 3: Loopback diagnostic (key for distinguishing software vs hardware)
    ret = i2s_test_loopback_diagnostic();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test suite FAILED at loopback diagnostic");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Test 4: RX microphone (3 seconds)
    ret = i2s_test_rx_microphone(3000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test suite FAILED at RX test");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Test 5: Full-duplex (3 seconds)
    ret = i2s_test_full_duplex(3000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test suite FAILED at full-duplex test");
        return ret;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "✓ ALL I2S TESTS PASSED");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}