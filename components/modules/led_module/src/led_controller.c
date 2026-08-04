#include "led_controller.h"

#if (LED_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_log.h"

#include "led_module_config.h"
#include "ws2812_module.h"

static const char* TAG = "led_controller";

static led_module_snapshot_t s_led_snapshot = {
    .mode        = LED_MODULE_MODE_OFF,
    .color       = {0},
    .brightness  = 0xFF,
    .initialized = false,
};

esp_err_t led_controller_init(void)
{
    if (s_led_snapshot.initialized)
    {
        ESP_LOGW(TAG, "LED controller already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

#if (LED_TYPE == LED_TYPE_WS2812)
    ret = ws2812_module_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize WS2812 driver: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    memset(&s_led_snapshot, 0, sizeof(s_led_snapshot));
    s_led_snapshot.initialized = true;
    s_led_snapshot.brightness  = 0xFF;

    ESP_LOGI(TAG, "LED controller initialized (type=%d)", LED_TYPE);
    return ESP_OK;
}

esp_err_t led_controller_deinit(void)
{
    if (!s_led_snapshot.initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

#if (LED_TYPE == LED_TYPE_WS2812)
    ret = ws2812_module_deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to deinitialize WS2812 driver: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    memset(&s_led_snapshot, 0, sizeof(s_led_snapshot));
    return ESP_OK;
}

esp_err_t led_controller_turn_off(void)
{
    if (!s_led_snapshot.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

#if (LED_TYPE == LED_TYPE_WS2812)
    ret = ws2812_module_clear();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to clear WS2812 output: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    s_led_snapshot.mode       = LED_MODULE_MODE_OFF;
    s_led_snapshot.color      = (ws2812_rgb_t){0};
    s_led_snapshot.brightness = 0;

    return ESP_OK;
}

esp_err_t led_controller_set_color(ws2812_rgb_t color, uint8_t brightness)
{
    if (!s_led_snapshot.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    esp_err_t ret = ws2812_module_set_brightness(brightness);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set LED brightness: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws2812_module_set_rgb(color.red, color.green, color.blue);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set LED color: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    s_led_snapshot.color      = color;
    s_led_snapshot.brightness = brightness;

    return ESP_OK;
}

esp_err_t led_controller_get_snapshot(led_module_snapshot_t* snapshot)
{
    if (!s_led_snapshot.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = s_led_snapshot;
    return ESP_OK;
}

bool led_controller_is_initialized(void)
{
    return s_led_snapshot.initialized;
}

#endif
