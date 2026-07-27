/*
 * bsp_uart.c - UART initialization and communication for FocusLamp BSP
 */

#include "bsp_uart.h"
#include "pin_config.h"
#include "system_config.h"

/* UART port assignments */
#define BSP_UART_EM3        UART_NUM_1      /* EM3 servo */
#define BSP_UART_LX         UART_NUM_2      /* LX servo */
/* UART3 (dual-board comm) disabled - no peer board connected */

esp_err_t bsp_uart_init(void)
{
    esp_err_t ret;

    /* -------------------- UART1: EM3 Servo -------------------- */
    uart_config_t uart_em3_cfg = {
        .baud_rate           = SERVO_BAUDRATE,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ret = uart_param_config(BSP_UART_EM3, &uart_em3_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(BSP_UART_EM3,
                       SERVO_TXD1_GPIO,   /* TX = GPIO8 */
                       SERVO_RXD1_GPIO,   /* RX = GPIO2 */
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(BSP_UART_EM3,
                              UART_COMM_RX_BUF_SIZE,
                              UART_COMM_TX_BUF_SIZE,
                              UART_COMM_QUEUE_SIZE,
                              NULL, 0);
    if (ret != ESP_OK) return ret;

    /* -------------------- UART2: LX Servo -------------------- */
    uart_config_t uart_lx_cfg = {
        .baud_rate           = SERVO_BAUDRATE,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ret = uart_param_config(BSP_UART_LX, &uart_lx_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(BSP_UART_LX,
                       SERVO_TXD2_GPIO,   /* TX = GPIO3 */
                       SERVO_RXD2_GPIO,   /* RX = GPIO20 */
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(BSP_UART_LX,
                              UART_COMM_RX_BUF_SIZE,
                              UART_COMM_TX_BUF_SIZE,
                              UART_COMM_QUEUE_SIZE,
                              NULL, 0);
    if (ret != ESP_OK) return ret;

    /* UART3 (dual-board comm) disabled - no peer board connected */

    return ESP_OK;
}

esp_err_t bsp_uart_write_bytes(uart_port_t uart_port, const uint8_t *data, size_t len)
{
    /* TODO: Validate uart_port is one of the initialized ports */
    int written = uart_write_bytes(uart_port, (const char *)data, len);
    if (written < 0) {
        return ESP_FAIL;
    }
    /* Wait for TX FIFO to drain */
    return uart_wait_tx_done(uart_port, pdMS_TO_TICKS(TIMEOUT_UART_TX));
}

int bsp_uart_read_bytes(uart_port_t uart_port, uint8_t *data, size_t len, TickType_t timeout)
{
    /* TODO: Validate uart_port is one of the initialized ports */
    return uart_read_bytes(uart_port, data, len, timeout);
}
