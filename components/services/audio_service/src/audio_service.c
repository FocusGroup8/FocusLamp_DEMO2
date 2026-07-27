/*
 * audio_service.c - Audio service implementation
 */

#include "audio_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "audio_driver.h"
#include "device_state.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_service";

static bool s_initialized = false;
static audio_service_state_t s_state = AUDIO_SERVICE_STATE_IDLE;
static uint8_t s_volume = 70;
static uint8_t s_volume_level = 3;
static bool s_muted = false;

/* Volume level mapping (0-5) to 0-100 */
static const uint8_t s_volume_level_map[AUDIO_VOLUME_LEVEL_COUNT] = {
    0,   /* level 0: mute */
    20,  /* level 1: quiet */
    40,  /* level 2: low */
    60,  /* level 3: normal */
    80,  /* level 4: loud */
    100, /* level 5: max */
};

static TaskHandle_t s_playback_task = NULL;
static bool s_playback_active = false;

/* ===================== Forward Declarations ===================== */

static void audio_service_publish_state(void);

/* ===================== Event Callbacks ===================== */

static void audio_service_event_handler(event_t *event, void *context)
{
    switch (event->type) {
        case EV_AUDIO_PLAY:
            if (event->data && event->data_size > 0) {
                audio_service_play((const char *)event->data);
            }
            break;
        case EV_AUDIO_STOP:
            audio_service_stop();
            break;
        case EV_AUDIO_PAUSE:
            audio_service_pause();
            break;
        case EV_AUDIO_RESUME:
            audio_service_resume();
            break;
        case EV_AUDIO_VOLUME_UP:
            audio_service_volume_up();
            break;
        case EV_AUDIO_VOLUME_DOWN:
            audio_service_volume_down();
            break;
        case EV_AUDIO_VOLUME_SET:
            if (event->data && event->data_size == sizeof(uint8_t)) {
                audio_service_set_volume(*(uint8_t *)event->data);
            }
            break;
        case EV_AUDIO_PLAYBACK_DONE:
            ESP_LOGI(TAG, "Playback completed");
            s_state = AUDIO_SERVICE_STATE_IDLE;
            audio_service_publish_state();
            break;
        case EV_AUDIO_ERROR:
            ESP_LOGE(TAG, "Audio error event received");
            s_state = AUDIO_SERVICE_STATE_ERROR;
            audio_service_publish_state();
            break;
        default:
            break;
    }
}

/* ===================== Internal ===================== */

static void audio_service_publish_state(void)
{
    event_t ev = {
        .type = EV_AUDIO_STATE_CHANGED,
        .data = &s_state,
        .data_size = sizeof(s_state),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
}

static void audio_service_sync_device_state(void)
{
    bool playing = (s_state == AUDIO_SERVICE_STATE_PLAYING);
    device_state_set_audio(s_volume_level, s_muted, playing);
}

/* ===================== Public API ===================== */

esp_err_t audio_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing audio service...");

    esp_err_t ret = audio_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio driver init failed");
        return ret;
    }

    /* Subscribe to audio events */
    ret = event_bus_subscribe(EV_AUDIO_PLAY, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_STOP, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_PAUSE, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_RESUME, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_VOLUME_UP, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_VOLUME_DOWN, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_VOLUME_SET, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_PLAYBACK_DONE, audio_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_AUDIO_ERROR, audio_service_event_handler, NULL);
    (void)ret;

    s_initialized = true;
    audio_service_sync_device_state();
    ESP_LOGI(TAG, "Audio service initialized");
    return ESP_OK;
}

esp_err_t audio_service_play(const char *uri)
{
    if (!s_initialized || uri == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing audio: %s", uri);

    if (strncmp(uri, "file://", 7) == 0) {
        return audio_service_play_file(uri + 7);
    } else if (uri[0] == '/') {
        return audio_service_play_file(uri);
    } else if (strncmp(uri, "tone://", 7) == 0) {
        uint16_t freq_hz = (uint16_t)atoi(uri + 7);
        return audio_service_play_tone(freq_hz, 500);
    }

    ESP_LOGE(TAG, "Unsupported URI scheme: %s", uri);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_service_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping audio playback");

    esp_err_t ret = audio_driver_stop();
    if (ret != ESP_OK) {
        return ret;
    }

    s_state = AUDIO_SERVICE_STATE_IDLE;
    audio_service_publish_state();

    /* Publish stop event */
    event_bus_publish_simple(EV_AUDIO_STOP);

    return ESP_OK;
}

esp_err_t audio_service_pause(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state != AUDIO_SERVICE_STATE_PLAYING) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Pausing audio playback");

    esp_err_t ret = audio_driver_stop();
    if (ret != ESP_OK) {
        return ret;
    }

    s_state = AUDIO_SERVICE_STATE_PAUSED;
    audio_service_publish_state();

    event_bus_publish_simple(EV_AUDIO_PAUSE);
    return ESP_OK;
}

esp_err_t audio_service_resume(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state != AUDIO_SERVICE_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Resuming audio playback");

    esp_err_t ret = audio_driver_start();
    if (ret != ESP_OK) {
        s_state = AUDIO_SERVICE_STATE_ERROR;
        audio_service_publish_state();
        return ret;
    }

    s_state = AUDIO_SERVICE_STATE_PLAYING;
    audio_service_publish_state();

    event_bus_publish_simple(EV_AUDIO_RESUME);
    return ESP_OK;
}

esp_err_t audio_service_set_volume(uint8_t vol)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (vol > 100) {
        vol = 100;
    }

    s_volume = vol;

    /* Find closest level */
    s_volume_level = AUDIO_VOLUME_LEVEL_MAX;
    for (int i = AUDIO_VOLUME_LEVEL_MAX; i >= 0; i--) {
        if (vol >= s_volume_level_map[i]) {
            s_volume_level = (uint8_t)i;
            break;
        }
    }

    ESP_LOGI(TAG, "Setting volume to %d (level %d)", vol, s_volume_level);

    esp_err_t ret = audio_driver_set_volume(s_muted ? 0 : vol);
    if (ret != ESP_OK) {
        return ret;
    }

    audio_service_sync_device_state();

    /* Publish volume set event */
    event_t ev = {
        .type = EV_AUDIO_VOLUME_SET,
        .data = &vol,
        .data_size = sizeof(vol),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);

    return ESP_OK;
}

esp_err_t audio_service_set_mute(bool mute)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_muted = mute;
    ESP_LOGI(TAG, "Setting mute: %d", mute);

    esp_err_t ret = audio_driver_set_mute(mute);
    audio_service_sync_device_state();
    return ret;
}

audio_service_state_t audio_service_get_state(void)
{
    return s_state;
}

uint8_t audio_service_get_volume_level(void)
{
    return s_volume_level;
}

esp_err_t audio_service_set_volume_level(uint8_t level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level > AUDIO_VOLUME_LEVEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    s_volume_level = level;
    s_volume = s_volume_level_map[level];

    ESP_LOGI(TAG, "Setting volume level %d -> %d%%", level, s_volume);

    return audio_driver_set_volume(s_muted ? 0 : s_volume);
}

esp_err_t audio_service_volume_up(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_volume_level >= AUDIO_VOLUME_LEVEL_MAX) {
        return ESP_OK;
    }
    return audio_service_set_volume_level(s_volume_level + 1);
}

esp_err_t audio_service_volume_down(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_volume_level == AUDIO_VOLUME_LEVEL_MIN) {
        return ESP_OK;
    }
    return audio_service_set_volume_level(s_volume_level - 1);
}

esp_err_t audio_service_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state = AUDIO_SERVICE_STATE_PLAYING;
    audio_service_publish_state();

    esp_err_t ret = audio_driver_play_tone(freq_hz, duration_ms);
    if (ret != ESP_OK) {
        s_state = AUDIO_SERVICE_STATE_ERROR;
        audio_service_publish_state();
        return ret;
    }

    if (duration_ms > 0) {
        s_state = AUDIO_SERVICE_STATE_IDLE;
        audio_service_publish_state();
    }

    return ESP_OK;
}

esp_err_t audio_service_play_alert(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing alert beep");

    /* Short 1kHz + 1.5kHz double beep */
    esp_err_t ret = audio_service_play_tone(1000, 150);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    return audio_service_play_tone(1500, 150);
}

esp_err_t audio_service_play_file(const char *path)
{
    if (!s_initialized || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Playing file: %s", path);

    /* Stop any ongoing tone to free the driver */
    audio_driver_stop_tone();

    /* Open file from filesystem */
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Check for WAV header ('RIFF' marker) and skip it if present */
    uint8_t header[44];
    size_t header_read = fread(header, 1, sizeof(header), fp);
    if (header_read >= 4 &&
        header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F') {
        ESP_LOGI(TAG, "WAV header detected, skipping 44 bytes");
        /* Header already consumed, position is after 44 bytes */
    } else {
        /* Treat as raw PCM: rewind and play from start */
        fseek(fp, 0, SEEK_SET);
    }

    s_state = AUDIO_SERVICE_STATE_PLAYING;
    s_playback_active = true;
    s_playback_task = xTaskGetCurrentTaskHandle();
    audio_service_publish_state();

    esp_err_t ret = audio_driver_start();
    if (ret != ESP_OK) {
        fclose(fp);
        s_playback_active = false;
        s_playback_task = NULL;
        s_state = AUDIO_SERVICE_STATE_ERROR;
        audio_service_publish_state();
        return ret;
    }

    /* Stream PCM data to audio driver in chunks */
    uint8_t pcm_buffer[512];
    size_t bytes_read;
    while ((bytes_read = fread(pcm_buffer, 1, sizeof(pcm_buffer), fp)) > 0) {
        ret = audio_driver_write(pcm_buffer, bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    fclose(fp);

    audio_driver_stop();

    s_playback_active = false;
    s_playback_task = NULL;
    s_state = AUDIO_SERVICE_STATE_IDLE;
    audio_service_publish_state();

    ESP_LOGI(TAG, "File playback finished");
    return ESP_OK;
}

esp_err_t audio_service_play_tts(const char *text)
{
    if (!s_initialized || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TTS requested: %s", text);

    /* Look for a pre-generated TTS file based on a simple hash of the text */
    char path[64];
    uint32_t hash = 5381;
    for (const char *p = text; *p != '\0'; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)(*p);
    }
    snprintf(path, sizeof(path), "/spiffs/tts/%08lx.wav", (unsigned long)hash);

    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        fclose(fp);
        return audio_service_play_file(path);
    }

    /* No pre-generated file: fall back to a spoken-style alert tone */
    ESP_LOGW(TAG, "No TTS file found for '%s', playing alert instead", text);
    return audio_service_play_alert();
}