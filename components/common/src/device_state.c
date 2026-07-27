/*
 * device_state.c - Unified device state implementation
 */

#include "device_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

static device_state_t s_state = {0};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;

esp_err_t device_state_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_state, 0, sizeof(s_state));

    s_initialized = true;
    return ESP_OK;
}

esp_err_t device_state_get(device_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    *state = s_state;
    state->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_app_state(app_state_t state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.app_state = state;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_light(uint8_t level, uint8_t brightness, uint8_t r, uint8_t g, uint8_t b, bool on)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.light.level       = level;
    s_state.light.brightness  = brightness;
    s_state.light.r           = r;
    s_state.light.g           = g;
    s_state.light.b           = b;
    s_state.light.on          = on;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_audio(uint8_t volume_level, bool muted, bool playing)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.audio.level    = volume_level;
    s_state.audio.muted    = muted;
    s_state.audio.playing  = playing;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_radar(bool present, float heart_rate, float breath_rate,
                                  float distance_cm, float hrv_sdnn, float hrv_rmssd)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.radar.present          = present;
    s_state.radar.heart_rate_bpm   = heart_rate;
    s_state.radar.breath_rate_bpm  = breath_rate;
    s_state.radar.distance_cm      = distance_cm;
    s_state.radar.hrv_sdnn         = hrv_sdnn;
    s_state.radar.hrv_rmssd        = hrv_rmssd;
    s_state.radar.heart_rate_valid = (heart_rate > 0.0f);
    s_state.radar.distance_valid   = (distance_cm > 0.0f);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_servo(int16_t em3_pos, const int16_t lx_pos[4], bool valid)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.servo.em3_pos = em3_pos;
    if (lx_pos != NULL) {
        memcpy(s_state.servo.lx_pos, lx_pos, sizeof(s_state.servo.lx_pos));
    }
    s_state.servo.valid = valid;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_ambient_light(uint8_t level, float lux)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.ambient_light.level = level;
    s_state.ambient_light.lux   = lux;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t device_state_set_key(uint8_t point, uint8_t event, uint32_t event_time_ms, uint8_t active_points)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_state.key.last_point       = point;
    s_state.key.last_event       = event;
    s_state.key.last_event_time_ms = event_time_ms;
    s_state.key.active_points    = active_points;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
