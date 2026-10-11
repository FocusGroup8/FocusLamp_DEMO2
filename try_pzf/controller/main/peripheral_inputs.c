#include "peripheral_inputs.h"
#include "radar_ld6002.h"
#include "touch_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "local_inputs";

static void local_touch_task(void *arg)
{
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(10);
    if (!period) period = 1;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        touch_service_process();
        vTaskDelayUntil(&last, period);
    }
}

void peripheral_inputs_init(void)
{
    esp_err_t ret = radar_ld6002_init();
    if (ret == ESP_OK) {
        ret = radar_ld6002_start();
        if (ret != ESP_OK) radar_ld6002_deinit();
    }
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Local radar unavailable: %s (other modules continue)", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Local radar reader ready; wait for checked frames (not hardware-confirmed yet)");

    /* Do not mirror old Touch A long-press shutdown/LCD/mode handlers. */
    ret = touch_service_init();
    if (ret == ESP_OK) ret = touch_service_start();
    if (ret == ESP_OK && xTaskCreate(local_touch_task, "local_touch", 2048,
                                    NULL, 4, NULL) != pdPASS) {
        touch_service_stop();
        ret = ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Local touch unavailable: %s (other modules continue)", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Local touch ready: A=9 B=22 C=10 D=23, scan=10ms; events/state only");
}
