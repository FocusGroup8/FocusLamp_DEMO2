/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_controller.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "LED_CONTROLLER";

static bool s_initialized   = false;
static uint8_t s_brightness = 0;  /* Current overall brightness 0-100 */
static uint8_t s_color_temp = 50; /* Current color temperature 0-100 (default neutral) */

/* LEDC channel mapping */
static const ledc_channel_t s_channels[LED_CHANNEL_MAX] = {
    LEDC_CHANNEL_0, /* LED_CHANNEL_A - warm white (暖光) */
    LEDC_CHANNEL_1, /* LED_CHANNEL_B - cool white (冷光) */
};

static const gpio_num_t s_gpios[LED_CHANNEL_MAX] = {
    LED_GPIO_A,
    LED_GPIO_B,
};

static esp_err_t ledc_init(void)
{
    /* Configure LEDC timer - use TIMER_1 to avoid potential conflict with backlight TIMER_0 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_PWM_DUTY_RES,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = LED_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure both LED channels */
    for (int i = 0; i < LED_CHANNEL_MAX; i++) {
        ledc_channel_config_t chan_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = s_channels[i],
            .timer_sel  = LEDC_TIMER_1,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = s_gpios[i],
            .duty       = 0,
            .hpoint     = 0,
        };
        ret = ledc_channel_config(&chan_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LEDC channel %d config failed: %s", i, esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

static uint32_t percent_to_duty(uint8_t percent)
{
    if (percent > LED_BRIGHTNESS_MAX) {
        percent = LED_BRIGHTNESS_MAX;
    }
    /* Scale so 100% brightness = LED_MAX_DUTY_PERCENT of full PWM range */
    uint32_t max_duty = (uint32_t)LED_PWM_MAX_DUTY * LED_MAX_DUTY_PERCENT / 100;
    return (uint32_t)percent * max_duty / LED_BRIGHTNESS_MAX;
}

static esp_err_t apply_cct_brightness(void)
{
    /* Calculate individual channel brightness based on CCT and overall brightness
     * color_temp: 0 = warmest (only LED_A/暖光), 100 = coolest (only LED_B/冷光)
     * At 50: both channels equal
     *
     * Hardware mapping:
     *   LED_CHANNEL_A (GPIO 29) = warm white (暖光)
     *   LED_CHANNEL_B (GPIO 28) = cool white (冷光)
     */
    uint8_t warm_ratio = LED_CCT_MAX - s_color_temp; /* higher color_temp = less warm */
    uint8_t cool_ratio = s_color_temp;               /* higher color_temp = more cool */

    uint8_t brightness_a = (uint16_t)s_brightness * warm_ratio / LED_CCT_MAX;
    uint8_t brightness_b = (uint16_t)s_brightness * cool_ratio / LED_CCT_MAX;

    uint32_t duty_a = percent_to_duty(brightness_a);
    uint32_t duty_b = percent_to_duty(brightness_b);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A], duty_a));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A]));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B], duty_b));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B]));

    ESP_LOGD(TAG, "CCT=%d%%, brightness=%d%% -> A=%d%%(duty=%lu), B=%d%%(duty=%lu)", s_color_temp, s_brightness,
             brightness_a, duty_a, brightness_b, duty_b);

    return ESP_OK;
}

esp_err_t led_controller_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LED controller (GPIO_A=%d, GPIO_B=%d)...", LED_GPIO_A, LED_GPIO_B);

    esp_err_t ret = ledc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install LEDC fade service (required for ledc_fade_start) */
    ret = ledc_fade_func_install(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC fade service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_brightness  = 0;
    s_color_temp  = 50;

    ESP_LOGI(TAG, "LED controller initialized");
    return ESP_OK;
}

esp_err_t led_controller_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    led_off();

    /* Stop LEDC timer */
    ledc_timer_rst(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1);

    s_initialized = false;
    ESP_LOGI(TAG, "LED controller deinitialized");
    return ESP_OK;
}

esp_err_t led_set_brightness(led_channel_t channel, uint8_t percent)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (channel >= LED_CHANNEL_MAX) {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > LED_BRIGHTNESS_MAX) {
        percent = LED_BRIGHTNESS_MAX;
    }

    uint32_t duty = percent_to_duty(percent);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[channel], duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[channel]));

    ESP_LOGI(TAG, "Channel %c brightness=%d%% (duty=%lu)", channel == LED_CHANNEL_A ? 'A' : 'B', percent, duty);
    return ESP_OK;
}

esp_err_t led_set_both_brightness(uint8_t brightness_a, uint8_t brightness_b)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duty_a = percent_to_duty(brightness_a);
    uint32_t duty_b = percent_to_duty(brightness_b);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A], duty_a));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A]));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B], duty_b));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B]));

    ESP_LOGI(TAG, "A=%d%%(duty=%lu), B=%d%%(duty=%lu)", brightness_a, duty_a, brightness_b, duty_b);
    return ESP_OK;
}

esp_err_t led_set_color_temp(uint8_t color_temp)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (color_temp > LED_CCT_MAX) {
        ESP_LOGE(TAG, "Invalid color temperature %d (max %d)", color_temp, LED_CCT_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    s_color_temp = color_temp;
    ESP_LOGI(TAG, "Color temperature set to %d%%", color_temp);
    return apply_cct_brightness();
}

esp_err_t led_set_brightness_with_cct(uint8_t brightness)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness > LED_BRIGHTNESS_MAX) {
        ESP_LOGE(TAG, "Invalid brightness %d (max %d)", brightness, LED_BRIGHTNESS_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    s_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to %d%% (CCT=%d%%)", brightness, s_color_temp);
    return apply_cct_brightness();
}

esp_err_t led_set_brightness_with_cct_fade(uint8_t brightness, uint32_t fade_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness > LED_BRIGHTNESS_MAX) {
        ESP_LOGE(TAG, "Invalid brightness %d (max %d)", brightness, LED_BRIGHTNESS_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    s_brightness = brightness;

    if (fade_ms == 0) {
        return apply_cct_brightness();
    }

    /* Calculate target duties based on CCT and new brightness */
    uint8_t warm_ratio     = LED_CCT_MAX - s_color_temp;
    uint8_t cool_ratio     = s_color_temp;
    uint8_t brightness_a   = (uint16_t)s_brightness * warm_ratio / LED_CCT_MAX;
    uint8_t brightness_b   = (uint16_t)s_brightness * cool_ratio / LED_CCT_MAX;
    uint32_t target_duty_a = percent_to_duty(brightness_a);
    uint32_t target_duty_b = percent_to_duty(brightness_b);

    /* Use LEDC fade for smooth transition */
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A], target_duty_a, fade_ms);
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B], target_duty_b, fade_ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A], LEDC_FADE_NO_WAIT);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B], LEDC_FADE_NO_WAIT);

    ESP_LOGI(TAG, "Brightness fading to %d%% over %lums (CCT=%d%%)", brightness, fade_ms, s_color_temp);
    return ESP_OK;
}

esp_err_t led_off(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A], 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_A]));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B], 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[LED_CHANNEL_B]));

    s_brightness = 0;
    ESP_LOGI(TAG, "LEDs off");
    return ESP_OK;
}

bool led_is_initialized(void)
{
    return s_initialized;
}
