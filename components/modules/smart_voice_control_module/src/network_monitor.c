#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "smart_voice_control_module.h"
#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"
#include "wifi_manager.h"

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "net_monitor";

// Network status
static smart_voice_network_status_t s_network_status = SMART_VOICE_NETWORK_UNKNOWN;
static SemaphoreHandle_t            s_network_mutex  = NULL;

// Callbacks
static smart_voice_network_status_callback_t s_network_callback  = NULL;
static void*                                 s_network_user_data = NULL;

// Retry timer handle for cloud reconnection
static esp_timer_handle_t s_retry_timer       = NULL;
static int                s_cloud_retry_count = 0;
static bool               s_retry_in_progress = false;

/**
 * @brief Internal callback from wifi_manager
 */
static void on_wifi_status_changed(bool connected)
{
    smart_voice_network_status_t old_status;

    if (xSemaphoreTake(s_network_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        old_status = s_network_status;

        if (connected)
        {
            s_network_status = SMART_VOICE_NETWORK_CONNECTED;
            ESP_LOGI(TAG, "WiFi connected");
        }
        else
        {
            s_network_status = SMART_VOICE_NETWORK_DISCONNECTED;
            ESP_LOGW(TAG, "WiFi disconnected");
        }

        xSemaphoreGive(s_network_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to acquire mutex in WiFi callback");
        return;
    }

    // Notify user via callback
    if (s_network_callback != NULL)
    {
        s_network_callback(s_network_status, s_network_user_data);
    }

    // Log status change
    ESP_LOGI(TAG, "Network status: %d -> %d", old_status, s_network_status);
}

/**
 * @brief Retry timer callback for cloud reconnection attempts
 */
static void retry_timer_callback(void* arg)
{
    (void)arg;

#if (SMART_VOICE_AUTO_FALLBACK == 1)
    if (wifi_manager_is_connected())
    {
        s_cloud_retry_count++;

        ESP_LOGI(TAG, "Cloud retry attempt %d/%d", s_cloud_retry_count,
                 SMART_VOICE_CLOUD_RETRY_COUNT);

        if (s_cloud_retry_count <= SMART_VOICE_CLOUD_RETRY_COUNT)
        {
            // TODO: Trigger cloud reconnection attempt here
            // This will be implemented in cloud_integration.c

            // Schedule next retry
            if (s_retry_timer != NULL && s_cloud_retry_count < SMART_VOICE_CLOUD_RETRY_COUNT)
            {
                esp_timer_start_once(s_retry_timer, SMART_VOICE_CLOUD_RETRY_DELAY_MS * 1000);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Max retries (%d) reached, staying in local mode",
                     SMART_VOICE_CLOUD_RETRY_COUNT);
            s_retry_in_progress = false;
        }
    }
    else
    {
        ESP_LOGW(TAG, "WiFi not connected, aborting cloud retries");
        s_retry_in_progress = false;
    }
#endif
}

/**
 * @brief Initialize network monitor
 */
esp_err_t network_monitor_init(void)
{
    esp_err_t ret = ESP_OK;

    // Create mutex
    if (s_network_mutex == NULL)
    {
        s_network_mutex = xSemaphoreCreateMutex();
        if (s_network_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Register WiFi status callback
    ret = wifi_manager_register_callback(on_wifi_status_changed);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register WiFi callback: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_network_mutex);
        s_network_mutex = NULL;
        return ret;
    }

    // Create retry timer
#if (SMART_VOICE_AUTO_FALLBACK == 1)
    esp_timer_create_args_t timer_args = {.callback        = retry_timer_callback,
                                          .arg             = NULL, // No argument needed
                                          .dispatch_method = ESP_TIMER_TASK,
                                          .name            = "cloud_retry_timer"};

    ret = esp_timer_create(&timer_args, &s_retry_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to create retry timer: %s", esp_err_to_name(ret));
        // Non-fatal, auto-retry will be disabled
        s_retry_timer = NULL;
    }
#endif

    // Get initial WiFi status
    if (wifi_manager_is_connected())
    {
        s_network_status = SMART_VOICE_NETWORK_CONNECTED;
    }
    else
    {
        s_network_status = SMART_VOICE_NETWORK_DISCONNECTED;
    }

    ESP_LOGI(TAG, "Network monitor initialized, initial status: %d", s_network_status);
    return ESP_OK;
}

/**
 * @brief Deinitialize network monitor
 */
void network_monitor_deinit(void)
{
    // Stop and delete retry timer
    if (s_retry_timer != NULL)
    {
        esp_timer_stop(s_retry_timer);
        esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }

    s_cloud_retry_count = 0;
    s_retry_in_progress = false;

    // Delete mutex
    if (s_network_mutex != NULL)
    {
        vSemaphoreDelete(s_network_mutex);
        s_network_mutex = NULL;
    }

    s_network_callback  = NULL;
    s_network_user_data = NULL;
    s_network_status    = SMART_VOICE_NETWORK_UNKNOWN;

    ESP_LOGI(TAG, "Network monitor deinitialized");
}

/**
 * @brief Get current network status (thread-safe)
 */
smart_voice_network_status_t network_monitor_get_status(void)
{
    smart_voice_network_status_t status = SMART_VOICE_NETWORK_UNKNOWN;

    if (xSemaphoreTake(s_network_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        status = s_network_status;
        xSemaphoreGive(s_network_mutex);
    }

    return status;
}

/**
 * @brief Check if network is connected
 */
bool network_monitor_is_connected(void)
{
    return (network_monitor_get_status() == SMART_VOICE_NETWORK_CONNECTED);
}

/**
 * @brief Set network status change callback
 */
esp_err_t network_monitor_set_callback(smart_voice_network_status_callback_t callback,
                                       void*                                 user_data)
{

    s_network_callback  = callback;
    s_network_user_data = user_data;

    ESP_LOGD(TAG, "Network status callback set");
    return ESP_OK;
}

/**
 * @brief Start cloud reconnection retry sequence
 */
esp_err_t network_monitor_start_cloud_retry(void)
{
#if (SMART_VOICE_AUTO_FALLBACK == 1)
    if (s_retry_in_progress)
    {
        ESP_LOGW(TAG, "Retry already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (!wifi_manager_is_connected())
    {
        ESP_LOGW(TAG, "Cannot start cloud retry: WiFi disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    s_cloud_retry_count = 0;
    s_retry_in_progress = true;

    if (s_retry_timer != NULL)
    {
        esp_err_t ret =
            esp_timer_start_once(s_retry_timer, SMART_VOICE_CLOUD_RETRY_DELAY_MS * 1000);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start retry timer: %s", esp_err_to_name(ret));
            s_retry_in_progress = false;
            return ret;
        }

        ESP_LOGI(TAG, "Cloud retry started (max %d retries, %d ms interval)",
                 SMART_VOICE_CLOUD_RETRY_COUNT, SMART_VOICE_CLOUD_RETRY_DELAY_MS);
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "Retry timer not available");
        s_retry_in_progress = false;
        return ESP_ERR_NOT_SUPPORTED;
    }
#else
    ESP_LOGW(TAG, "Auto fallback is disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Stop ongoing cloud retry
 */
void network_monitor_stop_cloud_retry(void)
{
    if (s_retry_timer != NULL)
    {
        esp_timer_stop(s_retry_timer);
    }

    s_cloud_retry_count = 0;
    s_retry_in_progress = false;

    ESP_LOGI(TAG, "Cloud retry stopped");
}

/**
 * @brief Check if cloud retry is in progress
 */
bool network_monitor_is_retrying(void)
{
    return s_retry_in_progress;
}

/**
 * @brief Print network monitor status
 */
esp_err_t network_monitor_print_status(void)
{
    smart_voice_network_status_t status = network_monitor_get_status();

    const char* status_str;
    switch (status)
    {
    case SMART_VOICE_NETWORK_DISCONNECTED:
        status_str = "DISCONNECTED";
        break;
    case SMART_VOICE_NETWORK_CONNECTED:
        status_str = "CONNECTED";
        break;
    case SMART_VOICE_NETWORK_RECONNECTING:
        status_str = "RECONNECTING";
        break;
    default:
        status_str = "UNKNOWN";
        break;
    }

    ESP_LOGI(TAG, "=== Network Monitor Status ===");
    ESP_LOGI(TAG, "  Status: %s", status_str);
    ESP_LOGI(TAG, "  WiFi Connected: %s", wifi_manager_is_connected() ? "YES" : "NO");
    ESP_LOGI(TAG, "  Retry In Progress: %s", s_retry_in_progress ? "YES" : "NO");
    ESP_LOGI(TAG, "  Retry Count: %d/%d", s_cloud_retry_count, SMART_VOICE_CLOUD_RETRY_COUNT);
    ESP_LOGI(TAG, "============================");

    return ESP_OK;
}

#endif // SMART_VOICE_CONTROL_MODULE_ENABLE
