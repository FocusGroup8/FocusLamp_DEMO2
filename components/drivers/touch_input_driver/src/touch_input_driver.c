#include "touch_input_driver.h"

#include "touch_input_driver_config.h"

#if (TOUCH_INPUT_DRIVER_ENABLE == 1)

#include <string.h>

#include "esp_log.h"

#include "driver/gpio.h"

static const char* TAG = "touch_input";

static bool s_initialized              = false;
static int  s_pin_map[TOUCH_INPUT_MAX] = {TOUCH_INPUT_A_PIN, TOUCH_INPUT_B_PIN, TOUCH_INPUT_C_PIN};

esp_err_t touch_input_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Touch input already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << TOUCH_INPUT_A_PIN) | (1ULL << TOUCH_INPUT_B_PIN) | (1ULL << TOUCH_INPUT_C_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Touch input initialized successfully (A=%d, B=%d, C=%d)", TOUCH_INPUT_A_PIN,
             TOUCH_INPUT_B_PIN, TOUCH_INPUT_C_PIN);

    return ESP_OK;
}

esp_err_t touch_input_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Touch input not initialized");
        return ESP_OK;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Touch input deinitialized successfully");

    return ESP_OK;
}

touch_input_state_t touch_input_read(void)
{
    touch_input_state_t state;
    memset(&state, 0, sizeof(state));

    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Touch input not initialized");
        return state;
    }

    for (int i = 0; i < TOUCH_INPUT_MAX; i++)
    {
        state.pressed[i] = (gpio_get_level(s_pin_map[i]) == 1);
    }

    return state;
}

bool touch_input_is_pressed(touch_input_t input)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Touch input not initialized");
        return false;
    }

    if (input >= TOUCH_INPUT_MAX)
    {
        ESP_LOGW(TAG, "Invalid touch input: %d", input);
        return false;
    }

    return (gpio_get_level(s_pin_map[input]) == 1);
}

#endif