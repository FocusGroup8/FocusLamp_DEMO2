/*
 * console_init.c - Console REPL initialization for FocusLamp
 *
 * Initializes the ESP-IDF console Read-Eval-Print Loop (REPL).
 * Supports USB-Serial-JTAG (native USB) and UART (CH430) backends.
 */

#include "console_init.h"

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "console";

void console_init(void)
{
    esp_console_repl_t      *repl        = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_config.prompt             = "FocusLamp>";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* USB-Serial-JTAG (native USB connection) */
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
    ESP_LOGI(TAG, "Console initialized on USB-Serial-JTAG");
#else
    /* UART (CH430 or other USB-to-UART adapter) */
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
    ESP_LOGI(TAG, "Console initialized on UART");
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console REPL started. Type 'help' for available commands.");
}
