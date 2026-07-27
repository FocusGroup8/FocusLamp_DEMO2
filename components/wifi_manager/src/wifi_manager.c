/*
 * WiFi Manager - Modular WiFi management for ESP32-P4 + C5 (WiFi-Remote)
 *
 * Encapsulates: NVS init, ESP-Hosted SDIO transport, WiFi Remote STA,
 * connection management, scanning, and event dispatching.
 */

#include "wifi_manager.h"

#if (WIFI_MANAGER_ENABLE == 1)

#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_mgr";

/* Event group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_BITS (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)

/* Module state */
static bool s_initialized                = false;
static volatile bool s_connected         = false;
static SemaphoreHandle_t s_sem_hosted_up = NULL;
static EventGroupHandle_t s_event_group  = NULL;
static int s_retry_num                   = 0;

/* Exponential backoff reconnect state */
static int s_retry_delay_ms = 0;
#define WIFI_MANAGER_INITIAL_RETRY_DELAY_MS 1000
#define WIFI_MANAGER_MAX_RETRY_DELAY_MS 60000

/* ESP-Hosted heartbeat timeout detection */
static int s_heartbeat_miss_count = 0;
#define WIFI_MANAGER_HEARTBEAT_TIMEOUT_COUNT 3 /* 3 missed heartbeats = 30s */

/* Callback storage (one per event type) */
static wifi_manager_cb_t s_callbacks[WIFI_MANAGER_EVENT_HOSTED_TIMEOUT + 1] = {NULL};

/* Internal: dispatch event to registered callback */
static void dispatch_event(wifi_manager_event_t event, void *data)
{
    if (event <= WIFI_MANAGER_EVENT_HOSTED_TIMEOUT && s_callbacks[event]) {
        s_callbacks[event](event, data);
    }
}

/* ESP-Hosted event handler */
static void esp_hosted_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    switch (event_id) {
    case ESP_HOSTED_EVENT_CP_INIT: {
        esp_hosted_event_init_t *event = (esp_hosted_event_init_t *)event_data;
        ESP_LOGI(TAG, "Co-processor INIT, reset reason: %" PRIu16, event->reason);
        break;
    }
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        ESP_LOGI(TAG, "ESP-Hosted Transport is UP");
        if (s_sem_hosted_up) {
            xSemaphoreGive(s_sem_hosted_up);
        }
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
        s_heartbeat_miss_count = 0; /* Reset on successful heartbeat */
        break;
    }
    default:
        break;
    }
}

/* WiFi Remote event handler */
static void wifi_remote_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_REMOTE_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
            esp_wifi_remote_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (WIFI_MANAGER_AUTO_RECONNECT && s_retry_num < WIFI_MANAGER_MAX_RETRY) {
                /* Exponential backoff: delay before reconnect */
                if (s_retry_delay_ms == 0) {
                    s_retry_delay_ms = WIFI_MANAGER_INITIAL_RETRY_DELAY_MS;
                } else {
                    s_retry_delay_ms *= 2;
                    if (s_retry_delay_ms > WIFI_MANAGER_MAX_RETRY_DELAY_MS) {
                        s_retry_delay_ms = WIFI_MANAGER_MAX_RETRY_DELAY_MS;
                    }
                }
                ESP_LOGI(TAG, "Retry connecting (%d/%d) after %dms backoff", s_retry_num + 1, WIFI_MANAGER_MAX_RETRY,
                         s_retry_delay_ms);
                vTaskDelay(pdMS_TO_TICKS(s_retry_delay_ms));
                esp_wifi_remote_connect();
                s_retry_num++;
                dispatch_event(WIFI_MANAGER_EVENT_RECONNECTING, NULL);
            } else {
                /* Max retries reached or auto-reconnect disabled */
                ESP_LOGW(TAG, "Disconnected from AP (no more retries)");
                if (s_event_group) {
                    xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
                }
                dispatch_event(WIFI_MANAGER_EVENT_DISCONNECTED, NULL);
            }
            break;
        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WiFi scan completed");
            dispatch_event(WIFI_MANAGER_EVENT_SCAN_DONE, NULL);
            break;
        default:
            ESP_LOGD(TAG, "WiFi Remote event: %" PRId32, event_id);
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num      = 0;
        s_retry_delay_ms = 0;
        s_connected      = true;
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        dispatch_event(WIFI_MANAGER_EVENT_GOT_IP, NULL);
        dispatch_event(WIFI_MANAGER_EVENT_CONNECTED, NULL);
    }
}

/* Internal: init NVS */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, reformatting...");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    return ret;
}

/* Internal: init ESP-Hosted SDIO transport */
static esp_err_t init_esp_hosted(void)
{
    s_sem_hosted_up = xSemaphoreCreateBinary();
    if (s_sem_hosted_up == NULL) {
        ESP_LOGE(TAG, "Failed to create hosted semaphore");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, &esp_hosted_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Initializing ESP-Hosted (SDIO transport)...");
    esp_hosted_init();
    esp_hosted_connect_to_slave();

    ESP_LOGI(TAG, "Waiting for SDIO transport ready...");
    if (xSemaphoreTake(s_sem_hosted_up, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for SDIO transport (30s)");
        ESP_LOGE(TAG, "Check: 1) C5 slave firmware  2) SDIO wiring  3) Power/GND");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "SDIO transport is ready");

    /* Log co-processor firmware version */
    esp_hosted_coprocessor_fwver_t fwver;
    if (ESP_OK == esp_hosted_get_coprocessor_fwversion(&fwver)) {
        ESP_LOGI(TAG, "Co-processor FW: %" PRIu32 ".%" PRIu32 ".%" PRIu32, fwver.major1, fwver.minor1, fwver.patch1);
    }

    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    esp_err_t err;

    /* Step 1: Init NVS */
    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "[1/5] NVS initialized");

    /* Step 2: Init network interface */
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "[2/5] Network interface initialized");

    /* Step 3: Init ESP-Hosted SDIO transport */
    err = init_esp_hosted();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3/5] ESP-Hosted init failed");
        return err;
    }
    ESP_LOGI(TAG, "[3/5] ESP-Hosted SDIO transport ready");

    /* Step 4: Configure heartbeat */
    if (ESP_OK == esp_hosted_configure_heartbeat(true, 10)) {
        ESP_LOGI(TAG, "[4/5] Heartbeat configured: interval=10s");
    }

    /* Step 5: Init WiFi Remote STA */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_remote_create_default_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err                    = esp_wifi_remote_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Remote init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, &wifi_remote_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_remote_event_handler, NULL));

    /* Configure WiFi STA */
    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid               = WIFI_MANAGER_SSID,
                .password           = WIFI_MANAGER_PASSWORD,
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
    };

    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    ESP_LOGI(TAG, "[5/5] WiFi Remote STA initialized, connecting to SSID:%s", WIFI_MANAGER_SSID);

    /* Wait for connection result with timeout (instead of portMAX_DELAY) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_BITS, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_MANAGER_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to AP SSID:%s after %d retries", WIFI_MANAGER_SSID, WIFI_MANAGER_MAX_RETRY);
        /* Don't return error - module is initialized but not connected */
    } else {
        ESP_LOGE(TAG, "Timeout waiting for WiFi connection to SSID:%s (30s)", WIFI_MANAGER_SSID);
        /* Don't return error - module is initialized but not connected */
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized, call wifi_manager_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_connected) {
        ESP_LOGD(TAG, "Already connected");
        return ESP_OK;
    }

    s_retry_num   = 0;
    esp_err_t err = esp_wifi_remote_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_remote_disconnect();
    if (err == ESP_OK) {
        s_connected = false;
        ESP_LOGI(TAG, "Disconnected from AP");
    }
    return err;
}

esp_err_t wifi_manager_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGD(TAG, "Not initialized");
        return ESP_OK;
    }

    /* Stop WiFi */
    esp_wifi_remote_stop();
    esp_wifi_remote_deinit();

    /* Clean up event group */
    if (s_event_group) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    /* Clean up hosted semaphore */
    if (s_sem_hosted_up) {
        vSemaphoreDelete(s_sem_hosted_up);
        s_sem_hosted_up = NULL;
    }

    /* Reset state */
    s_connected            = false;
    s_initialized          = false;
    s_retry_num            = 0;
    s_retry_delay_ms       = 0;
    s_heartbeat_miss_count = 0;
    memset(s_callbacks, 0, sizeof(s_callbacks));

    ESP_LOGI(TAG, "WiFi Manager deinitialized");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_initialized && s_connected;
}

esp_err_t wifi_manager_scan(wifi_manager_ap_info_t *aps, uint16_t *count)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (aps == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_config = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_remote_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_remote_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        *count = 0;
        return ESP_OK;
    }

    /* Limit to requested count */
    uint16_t scan_count = ap_count;
    if (scan_count > *count) {
        scan_count = *count;
    }

    /* Use internal buffer for wifi_ap_record_t, then convert to public type */
    wifi_ap_record_t *ap_records = malloc(scan_count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Failed to allocate scan buffer");
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_remote_scan_get_ap_records(&scan_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Get AP records failed: %s", esp_err_to_name(err));
        free(ap_records);
        return err;
    }

    /* Convert internal type to public type */
    for (int i = 0; i < scan_count; i++) {
        memcpy(aps[i].ssid, ap_records[i].ssid, sizeof(ap_records[i].ssid));
        aps[i].ssid[32] = '\0';
        aps[i].rssi     = ap_records[i].rssi;
        aps[i].channel  = ap_records[i].primary;
        aps[i].authmode = ap_records[i].authmode;
        memcpy(aps[i].bssid, ap_records[i].bssid, 6);
    }

    free(ap_records);
    *count = scan_count;
    ESP_LOGI(TAG, "Scan found %d AP(s)", scan_count);
    return ESP_OK;
}

esp_err_t wifi_manager_get_info(wifi_manager_info_t *info)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->is_connected = s_connected;

    if (!s_connected) {
        return ESP_OK;
    }

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_remote_sta_get_ap_info(&ap_info);
    if (err == ESP_OK) {
        memcpy(info->ssid, ap_info.ssid, sizeof(ap_info.ssid));
        info->ssid[32] = '\0';
        info->rssi     = ap_info.rssi;
        info->channel  = ap_info.primary;
    } else {
        ESP_LOGW(TAG, "Get AP info failed: %s", esp_err_to_name(err));
    }

    /* Get IP info */
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    return ESP_OK;
}

esp_err_t wifi_manager_register_handler(wifi_manager_event_t event, wifi_manager_cb_t cb)
{
    if (event > WIFI_MANAGER_EVENT_HOSTED_TIMEOUT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_callbacks[event] = cb;
    return ESP_OK;
}

#endif /* WIFI_MANAGER_ENABLE */
