/**
 * @file light_control_module.c
 * @brief Light control module implementation
 *
 * This file implements the automatic light control module.
 * The module controls lights via UART communication with an ESP32-C3 device.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 2.0.0
 */

#include "light_control_module.h"

#include "light_control_module_config.h"

#if (LIGHT_CONTROL_ENABLE == 1)

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uart_light_controller.h"

#if (LIGHT_CONTROL_AUTO_MODE == 1)
#include "light_sensor_driver.h"
#endif

static const char* TAG = "light_control";

static TaskHandle_t s_task_handle = NULL;
static bool         s_initialized = false;
static bool         s_auto_mode   = true;
static bool         s_lamp_on     = false;

#if (LIGHT_CONTROL_AUTO_MODE == 1)

static void light_control_task(void* arg)
{
    ESP_LOGI(TAG, "Light control task started");

    while (1)
    {
        if (!s_auto_mode)
        {
            vTaskDelay(pdMS_TO_TICKS(LIGHT_CONTROL_TASK_INTERVAL_MS));
            continue;
        }

        bool is_dark   = false;
        bool is_bright = false;

        esp_err_t ret = light_sensor_driver_is_dark(&is_dark);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to check if dark");
            vTaskDelay(pdMS_TO_TICKS(LIGHT_CONTROL_TASK_INTERVAL_MS));
            continue;
        }

        ret = light_sensor_driver_is_bright(&is_bright);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to check if bright");
            vTaskDelay(pdMS_TO_TICKS(LIGHT_CONTROL_TASK_INTERVAL_MS));
            continue;
        }

        float lux = 0.0f;
        light_sensor_driver_read(&lux);
        ESP_LOGD(TAG, "Lux: %.2f, Dark: %d, Bright: %d, Lamp: %d", lux, is_dark, is_bright,
                 s_lamp_on);

        if (is_dark && !s_lamp_on)
        {
            uart_light_params_t params = {
                .warm  = LIGHT_CONTROL_DEFAULT_WARM,
                .cold  = LIGHT_CONTROL_DEFAULT_COLD,
                .red   = LIGHT_CONTROL_DEFAULT_COLOR_R,
                .green = LIGHT_CONTROL_DEFAULT_COLOR_G,
                .blue  = LIGHT_CONTROL_DEFAULT_COLOR_B,
            };
            ret = uart_light_controller_set_params(&params);
            if (ret == ESP_OK)
            {
                s_lamp_on = true;
                ESP_LOGI(TAG, "Environment dark, lamp turned ON (Lux: %.2f)", lux);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to turn on lamp");
            }
        }
        else if (is_bright && s_lamp_on)
        {
            ret = uart_light_controller_turn_off();
            if (ret == ESP_OK)
            {
                s_lamp_on = false;
                ESP_LOGI(TAG, "Environment bright, lamp turned OFF (Lux: %.2f)", lux);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to turn off lamp");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LIGHT_CONTROL_TASK_INTERVAL_MS));
    }
}

#endif

esp_err_t light_control_module_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Module already initialized");
        return ESP_OK;
    }

    if (!uart_light_controller_is_initialized())
    {
        esp_err_t ret = uart_light_controller_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize UART light controller: %s", esp_err_to_name(ret));
            return ret;
        }
    }

#if (LIGHT_CONTROL_AUTO_MODE == 1)
    if (!light_sensor_driver_is_initialized())
    {
        ESP_LOGE(TAG, "Light sensor driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(light_control_task, "light_ctrl", LIGHT_CONTROL_TASK_STACK_SIZE,
                                 NULL, LIGHT_CONTROL_TASK_PRIORITY, &s_task_handle);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create light control task");
        return ESP_ERR_NO_MEM;
    }

    s_auto_mode = true;
    ESP_LOGI(TAG, "Light control module initialized (auto mode enabled)");
#else
    s_auto_mode = false;
    ESP_LOGI(TAG, "Light control module initialized (manual mode only)");
#endif

    s_initialized = true;
    s_lamp_on     = false;

    return ESP_OK;
}

esp_err_t light_control_module_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Module not initialized");
        return ESP_OK;
    }

    if (s_task_handle != NULL)
    {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    s_initialized = false;
    s_auto_mode   = true;
    s_lamp_on     = false;

    ESP_LOGI(TAG, "Light control module deinitialized");
    return ESP_OK;
}

esp_err_t light_control_module_set_auto_mode(bool enable)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

#if (LIGHT_CONTROL_AUTO_MODE == 1)
    s_auto_mode = enable;
    ESP_LOGI(TAG, "Auto mode %s", enable ? "enabled" : "disabled");
    return ESP_OK;
#else
    if (enable)
    {
        ESP_LOGW(TAG, "Auto mode not available (light sensor disabled)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_auto_mode = false;
    return ESP_OK;
#endif
}

bool light_control_module_is_auto_mode(void)
{
    return s_auto_mode;
}

esp_err_t light_control_module_set_brightness(uint8_t brightness)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_auto_mode = false;

    float scale = brightness / 255.0f;

    uart_light_params_t params = {
        .warm  = (uint16_t)(LIGHT_CONTROL_DEFAULT_WARM * scale),
        .cold  = (uint16_t)(LIGHT_CONTROL_DEFAULT_COLD * scale),
        .red   = (uint8_t)(LIGHT_CONTROL_DEFAULT_COLOR_R * scale),
        .green = (uint8_t)(LIGHT_CONTROL_DEFAULT_COLOR_G * scale),
        .blue  = (uint8_t)(LIGHT_CONTROL_DEFAULT_COLOR_B * scale),
    };

    esp_err_t ret = uart_light_controller_set_params(&params);
    if (ret == ESP_OK)
    {
        s_lamp_on = (brightness > 0);
        ESP_LOGI(TAG, "Brightness set to %d", brightness);
    }

    return ret;
}

esp_err_t light_control_module_set_color(ws2812_rgb_t color)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_auto_mode = false;

    uart_light_params_t params = {
        .warm  = LIGHT_CONTROL_DEFAULT_WARM,
        .cold  = LIGHT_CONTROL_DEFAULT_COLD,
        .red   = color.red,
        .green = color.green,
        .blue  = color.blue,
    };

    esp_err_t ret = uart_light_controller_set_params(&params);
    if (ret == ESP_OK)
    {
        s_lamp_on = true;
        ESP_LOGI(TAG, "Color set to R:%d G:%d B:%d", color.red, color.green, color.blue);
    }

    return ret;
}

esp_err_t light_control_module_set_params(const uart_light_params_t* params)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (params == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    s_auto_mode = false;

    esp_err_t ret = uart_light_controller_set_params(params);
    if (ret == ESP_OK)
    {
        s_lamp_on = (params->red > 0 || params->green > 0 || params->blue > 0 || params->warm > 0 ||
                     params->cold > 0);
        ESP_LOGI(TAG, "Params set to W:%d C:%d R:%d G:%d B:%d", params->warm, params->cold,
                 params->red, params->green, params->blue);
    }

    return ret;
}

esp_err_t light_control_module_turn_off(void)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = uart_light_controller_turn_off();
    if (ret == ESP_OK)
    {
        s_lamp_on = false;
        ESP_LOGI(TAG, "Lamp turned off");
    }

    return ret;
}

esp_err_t light_control_module_get_lux(float* lux)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

#if (LIGHT_CONTROL_AUTO_MODE == 1)
    return light_sensor_driver_read(lux);
#else
    (void)lux;
    ESP_LOGW(TAG, "Light sensor not available");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool light_control_module_is_initialized(void)
{
    return s_initialized;
}

#endif
