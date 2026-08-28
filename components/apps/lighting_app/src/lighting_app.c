/*
 * lighting_app.c - Lighting application implementation
 */

#include "lighting_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"

static const char *TAG = "lighting_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;

/* ===================== Event Handlers ===================== */
static void lighting_app_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_LIGHTING && s_running) {
        lighting_app_stop();
    } else if (new_state == APP_STATE_LIGHTING && !s_running) {
        lighting_app_start();
    }
}

/* ===================== Public API ===================== */
esp_err_t lighting_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, lighting_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Lighting app initialized");
    return ESP_OK;
}

esp_err_t lighting_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    /* Apply default lighting settings (warm white, ~3% brightness)
     * brightness 有效范围 0-LED_BRIGHTNESS_MAX(30)，此处 1/30 = 3%（原 3/30=10% 的三分之一）。 */
    led_service_set_mode(LED_MODE_WARM);
    led_service_set_effect(LED_EFFECT_STEADY);
    led_service_set_brightness(1);

    s_running = true;
    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Lighting app started");
    return ESP_OK;
}

esp_err_t lighting_app_stop(void)
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
    ESP_LOGI(TAG, "Lighting app stopped");
    return ESP_OK;
}

esp_err_t lighting_app_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    return led_service_set_brightness(brightness);
}

esp_err_t lighting_app_set_colortemp(uint16_t temp)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }

    /* Map color temperature to LED mode: warm < 4000K, white >= 4000K */
    if (temp < 4000) {
        led_service_set_mode(LED_MODE_WARM);
    } else {
        led_service_set_mode(LED_MODE_WHITE);
    }
    led_service_set_effect(LED_EFFECT_STEADY);

    ESP_LOGI(TAG, "Set color temperature: %u K", temp);
    return ESP_OK;
}

esp_err_t lighting_app_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    return led_service_set_color(r, g, b);
}