/*
 * radar_driver.c - Radar sensor driver implementation
 */

#include "radar_driver.h"
#include "radar_driver_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "driver/uart.h"

static const char *TAG = "radar_drv";

/* Radar UART comes from project_config.h to avoid conflicting with console UART0 */
#define RADAR_RX_BUF_SIZE       RADAR_UART_RX_BUFFER_SIZE
#define RADAR_TX_BUF_SIZE       RADAR_UART_TX_BUFFER_SIZE
#define RADAR_EVENT_QUEUE_LEN   RADAR_UART_EVENT_QUEUE_LEN

static bool          s_initialized        = false;
static QueueHandle_t s_event_queue        = NULL;

/* ===================== UART Driver ===================== */

esp_err_t radar_driver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Radar driver already initialized");
        return ESP_OK;
    }

#if !RADAR_DRIVER_ENABLE
    ESP_LOGW(TAG, "Radar driver disabled (RADAR_DRIVER_ENABLE=0), skipping init");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    const uart_config_t uart_config = {
        .baud_rate  = RADAR_UART_BAUD_RATE,
        .data_bits  = RADAR_UART_DATA_BITS,
        .parity     = RADAR_UART_PARITY,
        .stop_bits  = RADAR_UART_STOP_BITS,
        .flow_ctrl  = RADAR_UART_FLOW_CTRL,
        .source_clk = RADAR_UART_SOURCE_CLK,
    };

    /* ESP-IDF v5.x recommended order: param_config -> set_pin -> install */
    esp_err_t ret = uart_param_config(RADAR_UART_PORT, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(RADAR_UART_PORT, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO,
                       RADAR_UART_RTS_GPIO, RADAR_UART_CTS_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(RADAR_UART_PORT, RADAR_RX_BUF_SIZE,
                              RADAR_TX_BUF_SIZE, RADAR_EVENT_QUEUE_LEN,
                              &s_event_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_flush_input(RADAR_UART_PORT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_flush_input failed: %s", esp_err_to_name(ret));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Radar UART initialized (port=%d, baud=%d, tx=%d, rx=%d)",
             RADAR_UART_PORT, RADAR_UART_BAUD_RATE, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO);
    return ESP_OK;
}

esp_err_t radar_driver_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = uart_driver_delete(RADAR_UART_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_delete failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_event_queue = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "Radar UART driver deinitialized");
    return ESP_OK;
}

QueueHandle_t radar_driver_get_event_queue(void)
{
    return s_event_queue;
}

int radar_driver_read_bytes(uint8_t *buffer, size_t length, TickType_t ticks_to_wait)
{
    if (!s_initialized || buffer == NULL || length == 0) {
        return -1;
    }
    return uart_read_bytes(RADAR_UART_PORT, buffer, length, ticks_to_wait);
}

esp_err_t radar_driver_flush_input(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_flush_input(RADAR_UART_PORT);
}

esp_err_t radar_driver_send_command(uint8_t *cmd, uint16_t len)
{
    if (cmd == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = uart_write_bytes(RADAR_UART_PORT, (const char *)cmd, len);
    if (ret != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

int radar_driver_read_data(uint8_t *buffer, uint16_t max_len)
{
    if (buffer == NULL || max_len == 0 || !s_initialized) {
        return -1;
    }
    return uart_read_bytes(RADAR_UART_PORT, buffer, max_len, pdMS_TO_TICKS(100));
}
