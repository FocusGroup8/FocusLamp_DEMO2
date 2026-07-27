/*
 * main.c - FocusLamp ESP-IDF main entry point
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "app_init.h"
#include "app_tasks.h"
#include "console_init.h"
#include "unified_help.h"
#include "app_console_commands.h"

static const char *TAG = "FocusLamp";

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  FocusLamp System Starting...");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info at startup */
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free internal heap: %lu bytes", esp_get_free_internal_heap_size());

    /* Initialize console REPL early to establish UART communication */
    console_init();

    /* Register unified help commands (help, modules) */
    register_unified_help_commands();

    /* Register application-level console commands */
    register_app_commands();

    /* 系统初始化 */
    app_init();

    /* 创建所有 FreeRTOS 任务 */
    app_tasks_create();

    /* Print final memory status */
    ESP_LOGI(TAG, "Free heap after init: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Min free heap: %lu bytes", esp_get_minimum_free_heap_size());

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System startup complete. Type 'help'");
    ESP_LOGI(TAG, "========================================");
}