/*
 * music_rhythm_app.c - Music rhythm application implementation
 */

#include "music_rhythm_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "arm_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"

static const char *TAG = "music_rhythm_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;
static uint8_t s_sensitivity = 5; /* Default medium sensitivity */

/* ===================== Audio Data Structure ===================== */
/* Audio energy data received via event bus. */
typedef struct {
    uint32_t timestamp;
    uint16_t *spectrum;     /* Frequency spectrum data */
    uint16_t  spectrum_len; /* Number of frequency bins */
    uint8_t   energy_level; /* Overall energy level (0-255) */
} audio_data_t;

/* ===================== Internal Helpers ===================== */
static void music_rhythm_app_process_audio(const audio_data_t *audio)
{
    if (audio == NULL) {
        return;
    }

    uint8_t energy = audio->energy_level;

    /* Apply sensitivity scaling */
    uint32_t scaled = (uint32_t)energy * (s_sensitivity + 1) / 10;
    uint8_t effect_intensity = (scaled > 255) ? 255 : (uint8_t)scaled;

    /* Map energy to LED color */
    uint8_t r = (effect_intensity > 128) ? (effect_intensity - 128) * 2 : 0;
    uint8_t g = (effect_intensity < 128) ? effect_intensity * 2 : 0;
    uint8_t b = 0;
    led_service_set_color(r, g, b);

    /* Map energy to brightness */
    led_service_set_brightness(effect_intensity);

    /* Trigger arm actions on strong beats */
    if (effect_intensity > 200) {
        arm_service_start_action();
        ESP_LOGD(TAG, "Beat detected, intensity=%u", effect_intensity);
    }

    ESP_LOGD(TAG, "Audio processed: energy=%u, intensity=%u", energy, effect_intensity);
}

/* ===================== Event Handlers ===================== */
static void music_rhythm_app_on_audio_data(event_t *event, void *context)
{
    if (event == NULL || event->data == NULL) {
        return;
    }
    if (!s_running) {
        return;
    }
    const audio_data_t *audio = (const audio_data_t *)event->data;
    music_rhythm_app_process_audio(audio);
}

static void music_rhythm_app_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_MUSIC_RHYTHM && s_running) {
        music_rhythm_app_stop();
    }
}

/* ===================== Public API ===================== */
esp_err_t music_rhythm_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, music_rhythm_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    ret = event_bus_subscribe(EV_AUDIO_STATE_CHANGED, music_rhythm_app_on_audio_data, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EV_AUDIO_STATE_CHANGED");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Music rhythm app initialized");
    return ESP_OK;
}

esp_err_t music_rhythm_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    s_running = true;

    /* Set LED to music rhythm effect */
    led_service_set_effect(LED_EFFECT_MUSIC_RHYTHM);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Music rhythm app started (sensitivity=%u)", s_sensitivity);
    return ESP_OK;
}

esp_err_t music_rhythm_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    led_service_turn_off();
    s_running = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Music rhythm app stopped");
    return ESP_OK;
}

esp_err_t music_rhythm_app_set_sensitivity(uint8_t level)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (level > 10) {
        return ERR_INVALID_PARAM;
    }

    s_sensitivity = level;
    ESP_LOGI(TAG, "Sensitivity set to %u", level);
    return ESP_OK;
}