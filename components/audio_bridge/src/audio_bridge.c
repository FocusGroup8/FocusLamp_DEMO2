/*
 * Audio Bridge Component
 *
 * Bridges esp_xiaozhi's audio_callback (OPUS-encoded TTS data) to I2S hardware playback.
 * Also provides a microphone recording interface for future voice input.
 *
 * I2S init pattern migrated from mipi_dsi project's i2s_driver.c.
 */

#include "audio_bridge.h"
#include "audio_bridge_config.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO_BRIDGE";

#if (AUDIO_BRIDGE_ENABLE == 1)

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static bool s_initialized = false;
static int s_volume = CONFIG_AUDIO_BRIDGE_DEFAULT_VOLUME;
static audio_bridge_config_t s_config;

esp_err_t audio_bridge_init(const audio_bridge_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_volume = CONFIG_AUDIO_BRIDGE_DEFAULT_VOLUME;

    // Step 1: Create channel pair (full-duplex)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 16;
    chan_cfg.dma_frame_num = 960;
    chan_cfg.auto_clear    = true;

    ESP_LOGI(TAG, "Creating I2S channels: port=0, dma_desc=%d, dma_frame=%d",
             chan_cfg.dma_desc_num, chan_cfg.dma_frame_num);

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel pair: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S channel pair created (TX=%p, RX=%p)", s_tx_handle, s_rx_handle);

    // Step 2: Configure TX and RX with the SAME std_cfg (full-duplex requirement)
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = config->bclk_gpio,
                .ws   = config->ws_gpio,
                .dout = config->dout_gpio,
                .din  = config->din_gpio,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_LOGI(TAG, "Config: PHILIPS, MONO, LEFT, %dHz, 32-bit", config->sample_rate);
    ESP_LOGI(TAG, "GPIO: BCLK=%d, WS=%d, DOUT=%d, DIN=%d",
             config->bclk_gpio, config->ws_gpio, config->dout_gpio, config->din_gpio);

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        i2s_del_channel(s_rx_handle);
        s_tx_handle = NULL;
        s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "TX channel initialized");

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        i2s_del_channel(s_rx_handle);
        s_tx_handle = NULL;
        s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "RX channel initialized");

    // Step 3: Preload TX DMA buffers before enabling
    int32_t preload_buf[64] = {0};
    size_t preload_bytes    = sizeof(preload_buf);
    int preload_count       = 0;
    while (preload_bytes == sizeof(preload_buf)) {
        ret = i2s_channel_preload_data(s_tx_handle, preload_buf, sizeof(preload_buf), &preload_bytes);
        if (ret != ESP_OK) {
            break;
        }
        preload_count++;
    }
    ESP_LOGI(TAG, "Preloaded %d TX DMA buffers", preload_count);

    // Step 4: Enable TX first, then RX
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        i2s_del_channel(s_rx_handle);
        s_tx_handle = NULL;
        s_rx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        i2s_del_channel(s_rx_handle);
        s_tx_handle = NULL;
        s_rx_handle = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio bridge initialized successfully");
    return ESP_OK;
}

esp_err_t audio_bridge_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_OK;
    }

    // i2s_del_channel will automatically disable the channel if enabled
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }

    if (s_rx_handle) {
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Audio bridge deinitialized");
    return ESP_OK;
}

esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_initialized || s_tx_handle == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || bytes_written == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(s_tx_handle, data, len, bytes_written, ticks_to_wait);
}

esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_initialized || s_rx_handle == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || bytes_read == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_read(s_rx_handle, data, len, bytes_read, ticks_to_wait);
}

void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "TTS callback but not initialized, discarding %d bytes", len);
        return;
    }

    if (data == NULL || len <= 0) {
        return;
    }

    ESP_LOGD(TAG, "TTS callback: received %d bytes of OPUS audio data", len);

    // TODO: Need OPUS decoder (esp_audio_codec) to decode OPUS frames to PCM before I2S write.
    // Currently the data is OPUS-encoded, writing it directly to I2S would produce noise.
    // For a minimal working version: just log and discard.
    // Future implementation:
    //   1. Decode OPUS frame → PCM (16-bit signed mono)
    //   2. Apply volume scaling
    //   3. Expand 16-bit PCM to 32-bit for I2S 32-bit slot
    //   4. Call audio_bridge_write_pcm()
}

esp_err_t audio_bridge_set_volume(int volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Invalid volume: %d (must be 0-100)", volume_percent);
        return ESP_ERR_INVALID_ARG;
    }

    s_volume = volume_percent;
    ESP_LOGI(TAG, "Volume set to %d%%", s_volume);
    return ESP_OK;
}

int audio_bridge_get_volume(void)
{
    return s_volume;
}

#else /* AUDIO_BRIDGE_ENABLE == 0 */

esp_err_t audio_bridge_init(const audio_bridge_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
    (void)bytes_written;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
    (void)bytes_read;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
}

esp_err_t audio_bridge_set_volume(int volume_percent)
{
    (void)volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

int audio_bridge_get_volume(void)
{
    return 0;
}

#endif /* AUDIO_BRIDGE_ENABLE */
