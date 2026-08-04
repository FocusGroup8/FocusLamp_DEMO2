/**
 * @file uart_light_controller.c
 * @brief UART light controller implementation
 *
 * This file implements the UART light controller that communicates
 * with an ESP32-C3 device to control WS2812 LEDs and CCT LED panels.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#include "uart_light_controller.h"

#include "uart_light_controller_config.h"

#if (UART_LIGHT_CONTROLLER_ENABLE == 1)

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "driver/uart.h"

static const char* TAG = "uart_light_ctrl";

static bool s_initialized = false;

esp_err_t uart_light_controller_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Controller already initialized");
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate  = UART_LIGHT_CONTROLLER_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_LIGHT_CONTROLLER_PORT,
                                        UART_LIGHT_CONTROLLER_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(UART_LIGHT_CONTROLLER_PORT, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_LIGHT_CONTROLLER_PORT);
        return ret;
    }

    ret = uart_set_pin(UART_LIGHT_CONTROLLER_PORT, UART_LIGHT_CONTROLLER_TX_GPIO,
                       UART_LIGHT_CONTROLLER_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_LIGHT_CONTROLLER_PORT);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UART light controller initialized (UART%d, TX=%d, RX=%d, Baud=%d)",
             UART_LIGHT_CONTROLLER_PORT, UART_LIGHT_CONTROLLER_TX_GPIO,
             UART_LIGHT_CONTROLLER_RX_GPIO, UART_LIGHT_CONTROLLER_BAUD_RATE);

    return ESP_OK;
}

esp_err_t uart_light_controller_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Controller not initialized");
        return ESP_OK;
    }

    esp_err_t ret = uart_driver_delete(UART_LIGHT_CONTROLLER_PORT);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to delete UART driver: %s", esp_err_to_name(ret));
    }

    s_initialized = false;
    ESP_LOGI(TAG, "UART light controller deinitialized");
    return ESP_OK;
}

esp_err_t uart_light_controller_set_params(const uart_light_params_t* params)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (params == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char cmd[128];
    int  len = snprintf(cmd, sizeof(cmd), "W:%d,C:%d,R:%d,G:%d,B:%d\n", params->warm, params->cold,
                        params->red, params->green, params->blue);

    if (len < 0 || len >= sizeof(cmd))
    {
        ESP_LOGE(TAG, "Failed to format command");
        return ESP_FAIL;
    }

    int written = uart_write_bytes(UART_LIGHT_CONTROLLER_PORT, cmd, len);
    if (written != len)
    {
        ESP_LOGE(TAG, "Failed to send command (written=%d, expected=%d)", written, len);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sent: %s", cmd);
    return ESP_OK;
}

esp_err_t uart_light_controller_turn_off(void)
{
    uart_light_params_t params = {
        .warm  = 0,
        .cold  = 0,
        .red   = 0,
        .green = 0,
        .blue  = 0,
    };

    return uart_light_controller_set_params(&params);
}

bool uart_light_controller_is_initialized(void)
{
    return s_initialized;
}

#endif