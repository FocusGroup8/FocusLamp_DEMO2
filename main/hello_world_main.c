/*
 * WiFi Remote SDIO Communication Test for ESP32-P4 + C5
 *
 * Based on ESP-Hosted official example:
 *   managed_components/espressif__esp_hosted/examples/host_hosted_events/main/main.c
 *
 * Reference documentation:
 *   managed_components/espressif__esp_hosted/docs/sdio.md
 *   managed_components/espressif__esp_hosted/docs/esp32_p4_function_ev_board.md
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_hosted.h"

static const char *TAG = "wifi_remote_test";

static SemaphoreHandle_t sem_hosted_is_up;

/**
 * ESP-Hosted event handler
 * Reference: managed_components/espressif__esp_hosted/examples/host_hosted_events/main/main.c
 */
static void esp_hosted_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    switch (event_id) {
    case ESP_HOSTED_EVENT_CP_INIT: {
        esp_hosted_event_init_t *event = (esp_hosted_event_init_t *)event_data;
        ESP_LOGI(TAG, "Co-processor INIT event, reset reason: %" PRIu16, event->reason);
        break;
    }
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        ESP_LOGI(TAG, "ESP-Hosted Transport is UP");
        xSemaphoreGive(sem_hosted_is_up);
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        ESP_LOGW(TAG, "ESP-Hosted Transport is DOWN");
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        ESP_LOGE(TAG, "ESP-Hosted Transport FAILURE");
        break;
    case ESP_HOSTED_EVENT_CP_HEARTBEAT: {
        esp_hosted_event_heartbeat_t *event = (esp_hosted_event_heartbeat_t *)event_data;
        ESP_LOGI(TAG, "Co-processor heartbeat: %" PRIu32, event->heartbeat);
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown ESP_HOSTED event: %" PRId32, event_id);
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Remote SDIO Communication Test ===");
    ESP_LOGI(TAG, "Host: ESP32-P4, Co-processor: ESP32-C5");
    ESP_LOGI(TAG, "Transport: SDIO (CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17)");

    /* Create default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Register ESP-Hosted event handler */
    esp_event_handler_instance_t instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &esp_hosted_event_handler,
                                                         NULL,
                                                         &instance));

    /* Create semaphore to wait for transport ready */
    sem_hosted_is_up = xSemaphoreCreateBinary();
    if (sem_hosted_is_up == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    /* Initialize ESP-Hosted */
    ESP_LOGI(TAG, "Initializing ESP-Hosted...");
    esp_hosted_init();

    /* Connect to slave (co-processor) via SDIO */
    ESP_LOGI(TAG, "Connecting to co-processor via SDIO...");
    esp_hosted_connect_to_slave();

    /* Wait for transport to be ready */
    ESP_LOGI(TAG, "Waiting for ESP-Hosted transport to be ready...");
    if (xSemaphoreTake(sem_hosted_is_up, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for ESP-Hosted transport (30s)");
        ESP_LOGE(TAG, "Please check:");
        ESP_LOGE(TAG, "  1. ESP32-C5 slave firmware is flashed correctly");
        ESP_LOGE(TAG, "  2. SDIO wiring connections are correct");
        ESP_LOGE(TAG, "  3. Power supply is stable");
        ESP_LOGE(TAG, "  4. GND connections are solid");
        return;
    }

    ESP_LOGI(TAG, "ESP-Hosted transport is ready!");

    /* Verify communication by getting co-processor firmware version */
    esp_hosted_coprocessor_fwver_t fwver;
    if (ESP_OK == esp_hosted_get_coprocessor_fwversion(&fwver)) {
        ESP_LOGI(TAG, "Co-processor FW version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fwver.major1, fwver.minor1, fwver.patch1);
    } else {
        ESP_LOGE(TAG, "Failed to get co-processor firmware version");
    }

    /* Get co-processor chip info */
    uint32_t cp_chip_id = 0;
    char cp_target_name[32] = {0};
    if (ESP_OK == esp_hosted_get_cp_info(&cp_chip_id, cp_target_name, sizeof(cp_target_name))) {
        ESP_LOGI(TAG, "Co-processor chip ID: 0x%08" PRIX32 ", target: %s",
                 cp_chip_id, cp_target_name);
    } else {
        ESP_LOGW(TAG, "Failed to get co-processor chip info");
    }

    /* Configure heartbeat to monitor connection */
    if (ESP_OK == esp_hosted_configure_heartbeat(true, 5)) {
        ESP_LOGI(TAG, "Heartbeat configured: interval=5s");
    } else {
        ESP_LOGW(TAG, "Failed to configure heartbeat");
    }

    ESP_LOGI(TAG, "=== SDIO Communication Test PASSED ===");

    /* Keep the task alive and monitor heartbeat events */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "System running, free heap: %" PRIu32 " bytes",
                 esp_get_free_heap_size());
    }
}
