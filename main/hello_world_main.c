/*
 * WiFi Remote STA for ESP32-P4 + C5 (WT01P4C5-S1)
 *
 * Based on official examples:
 *   managed_components/espressif__esp_hosted/examples/host_hosted_events/main/main.c
 *   managed_components/espressif__esp_wifi_remote/examples/two_stations/main/remote_station.c
 *   managed_components/espressif__esp_wifi_remote/examples/two_stations/main/two_stations.c
 *
 * Reference documentation:
 *   managed_components/espressif__esp_hosted/docs/sdio.md
 *   managed_components/espressif__esp_wifi_remote/idf_v5.5/include/esp_wifi_remote_api.h
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ESP-Hosted headers */
#include "esp_hosted.h"

/* WiFi Remote headers - include injected headers first for correct type definitions */
#include "injected/esp_wifi.h"
#include "esp_wifi_remote.h"

/* WiFi configuration - configure via menuconfig or modify here */
#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY CONFIG_ESP_WIFI_MAX_RETRY

/* Event group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_BITS          (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)

static const char *TAG = "wifi_remote";

static SemaphoreHandle_t sem_hosted_is_up;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

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
        ESP_LOGD(TAG, "Co-processor heartbeat: %" PRIu32, event->heartbeat);
        break;
    }
    default:
        break;
    }
}

/**
 * WiFi Remote event handler
 * Reference: managed_components/espressif__esp_wifi_remote/examples/two_stations/main/remote_station.c
 *
 * Note: Use WIFI_REMOTE_EVENT (not WIFI_EVENT) for remote WiFi events
 */
static void wifi_remote_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    if (event_base == WIFI_REMOTE_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi Remote STA started, connecting...");
            esp_wifi_remote_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY) {
                esp_wifi_remote_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retry connecting to AP (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGW(TAG, "Disconnected from AP");
            break;
        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WiFi scan completed");
            break;
        default:
            ESP_LOGD(TAG, "WiFi Remote event: %" PRId32, event_id);
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Initialize NVS flash
 * Reference: managed_components/espressif__esp_wifi_remote/examples/two_stations/main/two_stations.c
 */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/**
 * Initialize ESP-Hosted transport layer (SDIO)
 * Must be called before WiFi Remote initialization
 */
static bool init_esp_hosted(void)
{
    /* Create semaphore to wait for transport ready */
    sem_hosted_is_up = xSemaphoreCreateBinary();
    if (sem_hosted_is_up == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }

    /* Register ESP-Hosted event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &esp_hosted_event_handler,
                                                         NULL,
                                                         NULL));

    /* Initialize ESP-Hosted and connect to slave via SDIO */
    ESP_LOGI(TAG, "Initializing ESP-Hosted (SDIO transport)...");
    esp_hosted_init();
    esp_hosted_connect_to_slave();

    /* Wait for SDIO transport to be ready */
    ESP_LOGI(TAG, "Waiting for SDIO transport ready...");
    if (xSemaphoreTake(sem_hosted_is_up, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for SDIO transport (30s)");
        ESP_LOGE(TAG, "Check: 1) C5 slave firmware  2) SDIO wiring  3) Power/GND");
        return false;
    }

    ESP_LOGI(TAG, "SDIO transport is ready");

    /* Log co-processor info */
    esp_hosted_coprocessor_fwver_t fwver;
    if (ESP_OK == esp_hosted_get_coprocessor_fwversion(&fwver)) {
        ESP_LOGI(TAG, "Co-processor FW: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fwver.major1, fwver.minor1, fwver.patch1);
    }

    return true;
}

/**
 * Perform WiFi scan and print results
 * Reference: managed_components/espressif__esp_wifi_remote/idf_v5.5/include/esp_wifi_remote_api.h
 *
 * Note: Scan result parsing may show incorrect data when host/co-processor
 * firmware versions are mismatched (Host 2.12.0 > Co-proc 2.2.0).
 * Upgrading the C5 co-processor firmware is recommended.
 */
static void wifi_scan_and_print(void)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_err_t ret = esp_wifi_remote_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_remote_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No access points found");
        return;
    }

    ESP_LOGI(TAG, "Found %d access point(s)", ap_count);

    /* Get AP records one by one for safer parsing */
    for (int i = 0; i < ap_count && i < 20; i++) {
        wifi_ap_record_t ap_record;
        uint16_t count = 1;
        ret = esp_wifi_remote_scan_get_ap_records(&count, &ap_record);
        if (ret != ESP_OK || count == 0) {
            break;
        }
        /* Print SSID as string with safe null-termination */
        char ssid_str[34] = {0};
        memcpy(ssid_str, ap_record.ssid, sizeof(ap_record.ssid));
        ssid_str[33] = '\0';
        ESP_LOGI(TAG, "  [%d] SSID:%-32s RSSI:%d Ch:%d Auth:%d",
                 i + 1, ssid_str, ap_record.rssi,
                 ap_record.primary, ap_record.authmode);
    }
}

/**
 * Print current AP connection info
 * Uses esp_wifi_remote_sta_get_ap_info to verify the connected AP
 */
static void print_connected_ap_info(void)
{
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));

    esp_err_t ret = esp_wifi_remote_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK) {
        char ssid_str[34] = {0};
        memcpy(ssid_str, ap_info.ssid, sizeof(ap_info.ssid));
        ssid_str[33] = '\0';
        ESP_LOGI(TAG, "Connected AP info:");
        ESP_LOGI(TAG, "  SSID: %s", ssid_str);
        ESP_LOGI(TAG, "  RSSI: %d dBm", ap_info.rssi);
        ESP_LOGI(TAG, "  Channel: %d", ap_info.primary);
        ESP_LOGI(TAG, "  Auth mode: %d", ap_info.authmode);
    } else {
        ESP_LOGW(TAG, "Failed to get connected AP info: %s", esp_err_to_name(ret));
    }
}

/**
 * Initialize WiFi Remote STA mode and connect to AP
 * Reference: managed_components/espressif__esp_wifi_remote/examples/two_stations/main/remote_station.c
 */
static void wifi_init_remote_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* Create default STA network interface for remote WiFi */
    esp_wifi_remote_create_default_sta();

    /* Initialize WiFi Remote */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));

    /* Register event handlers for remote WiFi */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_remote_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_remote_event_handler, NULL));

    /* Configure WiFi STA */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    ESP_LOGI(TAG, "WiFi Remote STA initialized, connecting to SSID:%s", WIFI_SSID);

    /* Wait for connection result */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_BITS,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to AP SSID:%s after %d retries", WIFI_SSID, WIFI_MAX_RETRY);
    } else {
        ESP_LOGE(TAG, "Unexpected event");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Remote STA for WT01P4C5-S1 ===");
    ESP_LOGI(TAG, "Host: ESP32-P4 | Co-processor: ESP32-C5");
    ESP_LOGI(TAG, "Transport: SDIO (CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17)");

    /* Step 1: Initialize NVS */
    init_nvs();
    ESP_LOGI(TAG, "[Step 1] NVS initialized");

    /* Step 2: Initialize network interface */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "[Step 2] Network interface initialized");

    /* Step 3: Initialize ESP-Hosted SDIO transport */
    if (!init_esp_hosted()) {
        ESP_LOGE(TAG, "[Step 3] ESP-Hosted initialization FAILED");
        return;
    }
    ESP_LOGI(TAG, "[Step 3] ESP-Hosted SDIO transport ready");

    /* Step 4: Configure heartbeat to monitor connection */
    if (ESP_OK == esp_hosted_configure_heartbeat(true, 10)) {
        ESP_LOGI(TAG, "[Step 4] Heartbeat configured: interval=10s");
    }

    /* Step 5: Initialize WiFi Remote STA and connect to AP */
    wifi_init_remote_sta();

    /* Step 6: Print connected AP info */
    print_connected_ap_info();

    /* Step 7: Perform WiFi scan to verify scanning capability */
    wifi_scan_and_print();

    ESP_LOGI(TAG, "=== WiFi Remote initialization complete ===");

    /* Main loop - monitor system health */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "System running, free heap: %" PRIu32 " bytes",
                 esp_get_free_heap_size());
    }
}
