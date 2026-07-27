/*
 * audio_driver.c - Audio driver for FocusLamp
 * Uses BSP I2S for hardware access with volume/gain/noise processing.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include "audio_driver.h"
#include "audio_bridge.h"
#include "pin_config.h"

static const char *TAG = "audio_driver";

/* ===================== Internal State ===================== */

static audio_state_t s_state = AUDIO_STATE_UNINIT;
static uint8_t       s_volume           = 70;   /* 0-100 */
static float         s_gain             = 1.0f; /* 0.1 - 10.0 */
static bool          s_mute             = false;
static bool          s_noise_reduction  = false;
static int           s_noise_threshold  = 500;
static bool s_loopback_running = false;

/* Tone generation */
static TaskHandle_t s_tone_task_handle = NULL;
static volatile bool s_tone_running = false;
#define TONE_SAMPLE_RATE 16000
#define TONE_CHUNK_SAMPLES 256

/* ===================== Audio Processing (inline) ===================== */

static int32_t apply_volume_gain(int32_t sample)
{
    float factor = (float)s_volume / 100.0f * s_gain;
    int64_t result = (int64_t)sample * factor;
    if (result > INT32_MAX) result = INT32_MAX;
    if (result < INT32_MIN) result = INT32_MIN;
    return (int32_t)result;
}

static int32_t apply_noise_gate(int32_t sample)
{
    if (s_noise_reduction && abs(sample) < s_noise_threshold) {
        return 0;
    }
    return sample;
}

static void process_pcm_buffer(int16_t *buffer, size_t samples)
{
    if (s_mute) {
        memset(buffer, 0, samples * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < samples; i++) {
        int32_t s = buffer[i];
        s = apply_noise_gate(s);
        s = apply_volume_gain(s);
        buffer[i] = (s > INT16_MAX) ? INT16_MAX :
                    (s < INT16_MIN) ? INT16_MIN : (int16_t)s;
    }
}

/* ===================== Loopback Task ===================== */

static void loopback_task(void *arg)
{
    int16_t buffer[128];
    ESP_LOGI(TAG, "Loopback task started");

    while (s_loopback_running) {
        /* Read from I2S (mic) via audio_bridge */
        size_t bytes_read = 0;
        esp_err_t ret_r = audio_bridge_read_pcm(buffer, sizeof(buffer), &bytes_read, pdMS_TO_TICKS(100));
        if (ret_r != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t samples = bytes_read / sizeof(int16_t);
        process_pcm_buffer(buffer, samples);

        /* Write back to I2S (speaker) via audio_bridge */
        size_t bw = 0;
        esp_err_t ret = audio_bridge_write_pcm(buffer, samples * sizeof(int16_t), &bw, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Loopback write failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Loopback task stopped");
    vTaskDelete(NULL);
}

/* ===================== Public API ===================== */

esp_err_t audio_driver_init(void)
{
    if (s_state != AUDIO_STATE_UNINIT) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }

    /* 初始化 audio_bridge：接管 I2S 全双工（16kHz/MONO/32bit），
     * 用于 OPUS 语音 TTS 解码与麦克风编码。引脚来自 pin_config.h。 */
    audio_bridge_config_t cfg = {
        .sample_rate = 16000,
        .bclk_gpio   = AUD_BCLK_GPIO,
        .ws_gpio     = AUD_LRC_GPIO,
        .dout_gpio   = AUD_DIN_GPIO,
        .din_gpio    = AUD_SD_GPIO,
    };
    esp_err_t ret = audio_bridge_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_bridge_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio driver initialized (via audio_bridge)");
    ESP_LOGI(TAG, "  I2S: BCLK=%d, LRC=%d, DIN=%d, SD=%d",
             AUD_BCLK_GPIO, AUD_LRC_GPIO, AUD_DIN_GPIO, AUD_SD_GPIO);
    return ESP_OK;
}

esp_err_t audio_driver_deinit(void)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        ESP_LOGW(TAG, "Audio driver not initialized");
        return ESP_OK;
    }

    if (s_loopback_running) {
        audio_driver_stop_loopback();
    }
    if (s_state == AUDIO_STATE_PLAYING) {
        audio_driver_stop();
    }

    audio_bridge_deinit();
    s_state = AUDIO_STATE_UNINIT;
    ESP_LOGI(TAG, "Audio driver deinitialized");
    return ESP_OK;
}

esp_err_t audio_driver_start(void)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        ESP_LOGE(TAG, "Audio driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    s_state = AUDIO_STATE_PLAYING;
    ESP_LOGI(TAG, "Audio driver started");
    return ESP_OK;
}

esp_err_t audio_driver_stop(void)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio driver stopped");
    return ESP_OK;
}

esp_err_t audio_driver_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == AUDIO_STATE_UNINIT) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mute) {
        return ESP_OK;
    }

    /* I2S 由 audio_bridge 接管（32bit slot）。
     * 注意：此处直接转发，16bit PCM 会被当作 32bit slot 数据，
     * 音质可能异常。TTS 播放走 audio_bridge_tts_callback 内部转换，
     * 此接口主要保留给 audio_service 的 WAV/tone 播放兼容。 */
    size_t bytes_written = 0;
    return audio_bridge_write_pcm(data, len, &bytes_written, portMAX_DELAY);
}

int audio_driver_read(uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) return -1;
    size_t bytes_read = 0;
    esp_err_t ret = audio_bridge_read_pcm(data, len, &bytes_read, pdMS_TO_TICKS(100));
    return (ret == ESP_OK) ? (int)bytes_read : -1;
}

esp_err_t audio_driver_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;
    ESP_LOGI(TAG, "Volume set to %d", vol);
    return ESP_OK;
}

uint8_t audio_driver_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_driver_set_mute(bool mute)
{
    s_mute = mute;
    ESP_LOGI(TAG, "Mute %s", mute ? "enabled" : "disabled");
    return ESP_OK;
}

audio_state_t audio_driver_get_state(void)
{
    return s_state;
}

esp_err_t audio_driver_set_gain(float gain)
{
    if (gain < 0.1f || gain > 10.0f) {
        ESP_LOGE(TAG, "Invalid gain: %.2f", gain);
        return ESP_ERR_INVALID_ARG;
    }
    s_gain = gain;
    ESP_LOGI(TAG, "Gain set to %.2f", gain);
    return ESP_OK;
}

esp_err_t audio_driver_set_noise_reduction(bool enable, int threshold)
{
    if (threshold < 0 || threshold > 10000000) {
        ESP_LOGE(TAG, "Invalid threshold: %d", threshold);
        return ESP_ERR_INVALID_ARG;
    }
    s_noise_reduction = enable;
    s_noise_threshold = threshold;
    ESP_LOGI(TAG, "Noise reduction %s, threshold: %d",
             enable ? "enabled" : "disabled", threshold);
    return ESP_OK;
}

esp_err_t audio_driver_get_status(audio_driver_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    status->state = s_state;
    status->sample_rate = 16000; /* Default, configurable via system_config */
    status->volume = (float)s_volume / 100.0f;
    status->gain = s_gain;
    status->noise_reduction_enabled = s_noise_reduction;
    status->noise_threshold = s_noise_threshold;
    return ESP_OK;
}

esp_err_t audio_driver_start_loopback(void)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        ESP_LOGE(TAG, "Audio driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_loopback_running) {
        ESP_LOGW(TAG, "Loopback already running");
        return ESP_OK;
    }

    s_loopback_running = true;
    s_state = AUDIO_STATE_LOOPBACK;

    BaseType_t ret = xTaskCreate(loopback_task, "audio_loopback", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        s_loopback_running = false;
        s_state = AUDIO_STATE_IDLE;
        ESP_LOGE(TAG, "Failed to create loopback task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio loopback started");
    return ESP_OK;
}

esp_err_t audio_driver_stop_loopback(void)
{
    if (!s_loopback_running) {
        ESP_LOGW(TAG, "Loopback not running");
        return ESP_OK;
    }

    s_loopback_running = false;
    vTaskDelay(pdMS_TO_TICKS(150));
    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio loopback stopped");
    return ESP_OK;
}

esp_err_t audio_driver_start_recording(void)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = AUDIO_STATE_RECORDING;
    ESP_LOGI(TAG, "Recording started");
    return ESP_OK;
}

esp_err_t audio_driver_stop_recording(void)
{
    if (s_state != AUDIO_STATE_RECORDING) {
        return ESP_OK;
    }
    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Recording stopped");
    return ESP_OK;
}

esp_err_t audio_driver_start_playback(void)
{
    return audio_driver_start();
}

esp_err_t audio_driver_stop_playback(void)
{
    return audio_driver_stop();
}

/* ===================== Tone Generation ===================== */

static void tone_task(void *arg)
{
    uint16_t freq_hz = (uint16_t)(uint32_t)arg;
    int16_t buffer[TONE_CHUNK_SAMPLES * 2]; /* stereo */
    uint32_t phase = 0;
    uint32_t phase_step = (uint32_t)((uint64_t)freq_hz * 65536 / TONE_SAMPLE_RATE);

    ESP_LOGI(TAG, "Tone task started: %d Hz", freq_hz);

    while (s_tone_running) {
        for (int i = 0; i < TONE_CHUNK_SAMPLES; i++) {
            int16_t sample = (int16_t)(sinf(2.0f * M_PI * phase / 65536.0f) * 28000.0f);
            buffer[i * 2]     = sample; /* Left */
            buffer[i * 2 + 1] = sample; /* Right */
            phase += phase_step;
        }

        size_t bw = 0;
        esp_err_t ret = audio_bridge_write_pcm(buffer, sizeof(buffer), &bw, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Tone write failed: %s", esp_err_to_name(ret));
        }
    }

    /* Fill remaining with silence */
    memset(buffer, 0, sizeof(buffer));
    for (int i = 0; i < 10; i++) {
        size_t bw2 = 0;
        audio_bridge_write_pcm(buffer, sizeof(buffer), &bw2, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "Tone task stopped");
    s_tone_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_driver_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    if (s_state == AUDIO_STATE_UNINIT) {
        ESP_LOGE(TAG, "Audio driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz == 0) {
        return audio_driver_stop_tone();
    }
    if (freq_hz < 20 || freq_hz > 20000) {
        ESP_LOGE(TAG, "Frequency out of range: %d Hz", freq_hz);
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop any ongoing tone */
    audio_driver_stop_tone();

    s_tone_running = true;
    s_state = AUDIO_STATE_PLAYING;

    BaseType_t ret = xTaskCreate(tone_task, "audio_tone", 4096,
                                 (void *)(uint32_t)freq_hz, 5, &s_tone_task_handle);
    if (ret != pdPASS) {
        s_tone_running = false;
        s_state = AUDIO_STATE_IDLE;
        ESP_LOGE(TAG, "Failed to create tone task");
        return ESP_ERR_NO_MEM;
    }

    if (duration_ms > 0) {
        /* Play for specified duration, then auto-stop */
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        audio_driver_stop_tone();
    }

    ESP_LOGI(TAG, "Playing tone: %d Hz%s", freq_hz,
             duration_ms > 0 ? "" : " (continuous)");
    return ESP_OK;
}

esp_err_t audio_driver_stop_tone(void)
{
    if (!s_tone_running) {
        return ESP_OK;
    }

    s_tone_running = false;

    /* Wait for task to exit */
    int wait = 0;
    while (s_tone_task_handle != NULL && wait < 50) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait++;
    }

    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Tone stopped");
    return ESP_OK;
}
