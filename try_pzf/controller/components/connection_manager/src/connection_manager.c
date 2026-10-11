/*
 * Connection Manager - Unified connection lifecycle management
 *
 * Monitors WiFi and WebSocket connection health, implements
 * automatic recovery with exponential backoff, and provides
 * a centralized event dispatching mechanism.
 */

#include "connection_manager.h"

#if (CONNECTION_MANAGER_ENABLE == 1)

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"
#include "websocket_manager.h"

#include <string.h>

static const char *TAG = "conn_mgr";

/* Module state */
static bool s_initialized = false;
static bool s_monitoring = false;
static conn_mgr_config_t s_config = {0};
static conn_mgr_stats_t s_stats = {0};
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_monitor_task = NULL;
static volatile bool s_monitor_running = false;

/* Per-layer reconnect state */
typedef struct {
    bool recovering;
    int retry_count;
    int current_delay_ms;
    int64_t last_attempt_ms;
} conn_mgr_reconnect_state_t;

static conn_mgr_reconnect_state_t s_reconnect_state[CONN_MGR_LAYER_MAX] = {0};

/* Callback storage */
static conn_mgr_cb_t s_callbacks[CONN_MGR_EVENT_MAX] = {NULL};

/* Internal: dispatch event */
static void dispatch_event(conn_mgr_event_t event, void *data)
{
    if (event < CONN_MGR_EVENT_MAX && s_callbacks[event]) {
        s_callbacks[event](event, data);
    }
}

/* Internal: get current time in ms */
static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Default configuration */
#define CONN_MGR_DEFAULT_WIFI_INIT_TIMEOUT_MS 30000
#define CONN_MGR_DEFAULT_WIFI_RECONNECT_MAX_DELAY_MS 60000
#define CONN_MGR_DEFAULT_WS_RECONNECT_MAX_DELAY_MS 60000
#define CONN_MGR_DEFAULT_MONITOR_INTERVAL_MS 5000
#define CONN_MGR_INITIAL_RECONNECT_DELAY_MS 1000

/*---------------------------------------------------------------
 * WiFi event handler (receives events from wifi_manager)
 *-------------------------------------------------------------*/
static void wifi_event_handler(wifi_manager_event_t event, void *data)
{
    if (!s_initialized) return;

    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTED:
    case WIFI_MANAGER_EVENT_GOT_IP:
        s_stats.layer_state[CONN_MGR_LAYER_WIFI] = CONN_MGR_STATE_CONNECTED;
        s_stats.last_wifi_connect_ms = get_time_ms();
        s_reconnect_state[CONN_MGR_LAYER_WIFI].recovering = false;
        s_reconnect_state[CONN_MGR_LAYER_WIFI].retry_count = 0;
        s_reconnect_state[CONN_MGR_LAYER_WIFI].current_delay_ms = 0;
        dispatch_event(CONN_MGR_EVENT_WIFI_CONNECTED, NULL);
        break;

    case WIFI_MANAGER_EVENT_DISCONNECTED:
        s_stats.layer_state[CONN_MGR_LAYER_WIFI] = CONN_MGR_STATE_DEGRADED;
        s_stats.wifi_reconnect_count++;
        dispatch_event(CONN_MGR_EVENT_WIFI_DISCONNECTED, NULL);
        break;

    default:
        break;
    }
}

/*---------------------------------------------------------------
 * WebSocket event handler (receives events from ws_manager)
 *-------------------------------------------------------------*/
static void ws_event_handler(ws_manager_event_t event, void *data)
{
    if (!s_initialized) return;

    switch (event) {
    case WS_MANAGER_EVENT_CONNECTED:
        s_stats.layer_state[CONN_MGR_LAYER_WS_CLIENT] = CONN_MGR_STATE_CONNECTED;
        s_stats.last_ws_connect_ms = get_time_ms();
        s_reconnect_state[CONN_MGR_LAYER_WS_CLIENT].recovering = false;
        s_reconnect_state[CONN_MGR_LAYER_WS_CLIENT].retry_count = 0;
        s_reconnect_state[CONN_MGR_LAYER_WS_CLIENT].current_delay_ms = 0;
        dispatch_event(CONN_MGR_EVENT_WS_CLIENT_CONNECTED, NULL);
        break;

    case WS_MANAGER_EVENT_DISCONNECTED:
        s_stats.layer_state[CONN_MGR_LAYER_WS_CLIENT] = CONN_MGR_STATE_DEGRADED;
        s_stats.ws_client_reconnect_count++;
        dispatch_event(CONN_MGR_EVENT_WS_CLIENT_DISCONNECTED, NULL);
        break;

    case WS_MANAGER_EVENT_ERROR:
        break;

    case WS_MANAGER_EVENT_SERVER_CONNECT:
        break;

    case WS_MANAGER_EVENT_SERVER_DISCONNECT:
        break;

    default:
        break;
    }
}

/*---------------------------------------------------------------
 * Health monitor task
 *-------------------------------------------------------------*/
static void monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Health monitor task started (interval=%dms)", s_config.monitor_interval_ms);

    while (s_monitor_running) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            /* Check WiFi health */
            if (s_stats.layer_state[CONN_MGR_LAYER_WIFI] == CONN_MGR_STATE_CONNECTED) {
                if (!wifi_manager_is_connected()) {
                    ESP_LOGW(TAG, "WiFi state mismatch: manager=CONNECTED but is_connected=false");
                    s_stats.layer_state[CONN_MGR_LAYER_WIFI] = CONN_MGR_STATE_DEGRADED;
                    dispatch_event(CONN_MGR_EVENT_WIFI_DISCONNECTED, NULL);
                }
            }

            /* Check WS server health */
            if (s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] == CONN_MGR_STATE_CONNECTED) {
                if (!ws_manager_server_is_running()) {
                    ESP_LOGW(TAG, "WS server state mismatch: reported running but httpd stopped");
                    s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] = CONN_MGR_STATE_FAILED;
                    dispatch_event(CONN_MGR_EVENT_WS_SERVER_STOPPED, NULL);
                    s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] = CONN_MGR_STATE_RECOVERING;
                    dispatch_event(CONN_MGR_EVENT_WS_SERVER_RECOVERING, NULL);
                    esp_err_t ret = ws_manager_server_start();
                    if (ret == ESP_OK) {
                        s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] = CONN_MGR_STATE_CONNECTED;
                        s_stats.ws_server_restart_count++;
                        dispatch_event(CONN_MGR_EVENT_WS_SERVER_STARTED, NULL);
                    } else {
                        s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] = CONN_MGR_STATE_FAILED;
                        ESP_LOGE(TAG, "WS server restart failed: %s", esp_err_to_name(ret));
                    }
                }
            }

            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(s_config.monitor_interval_ms));
    }

    ESP_LOGI(TAG, "Health monitor task stopped");
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t connection_manager_init(const conn_mgr_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (config) {
        s_config = *config;
    } else {
        s_config.wifi_init_timeout_ms = CONN_MGR_DEFAULT_WIFI_INIT_TIMEOUT_MS;
        s_config.wifi_reconnect_max_delay_ms = CONN_MGR_DEFAULT_WIFI_RECONNECT_MAX_DELAY_MS;
        s_config.ws_reconnect_max_delay_ms = CONN_MGR_DEFAULT_WS_RECONNECT_MAX_DELAY_MS;
        s_config.monitor_interval_ms = CONN_MGR_DEFAULT_MONITOR_INTERVAL_MS;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    for (int i = 0; i < CONN_MGR_LAYER_MAX; i++) {
        s_stats.layer_state[i] = CONN_MGR_STATE_IDLE;
    }
    memset(s_reconnect_state, 0, sizeof(s_reconnect_state));
    memset(s_callbacks, 0, sizeof(s_callbacks));

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_handler);

    ws_manager_register_handler(WS_MANAGER_EVENT_CONNECTED, ws_event_handler);
    ws_manager_register_handler(WS_MANAGER_EVENT_DISCONNECTED, ws_event_handler);
    ws_manager_register_handler(WS_MANAGER_EVENT_ERROR, ws_event_handler);
    ws_manager_register_handler(WS_MANAGER_EVENT_SERVER_CONNECT, ws_event_handler);
    ws_manager_register_handler(WS_MANAGER_EVENT_SERVER_DISCONNECT, ws_event_handler);

    s_initialized = true;
    ESP_LOGI(TAG, "Connection Manager initialized");
    return ESP_OK;
}

esp_err_t connection_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    connection_manager_stop();

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Connection Manager deinitialized");
    return ESP_OK;
}

esp_err_t connection_manager_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_monitoring) {
        ESP_LOGW(TAG, "Already monitoring");
        return ESP_OK;
    }

    s_stats.layer_state[CONN_MGR_LAYER_WIFI] =
        wifi_manager_is_connected() ? CONN_MGR_STATE_CONNECTED : CONN_MGR_STATE_DEGRADED;
    s_stats.layer_state[CONN_MGR_LAYER_WS_SERVER] =
        ws_manager_server_is_running() ? CONN_MGR_STATE_CONNECTED : CONN_MGR_STATE_IDLE;

    s_monitor_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(monitor_task, "conn_mon", 4096, NULL, 2, &s_monitor_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        s_monitor_running = false;
        return ESP_ERR_NO_MEM;
    }

    s_monitoring = true;
    ESP_LOGI(TAG, "Connection monitoring started");
    return ESP_OK;
}

esp_err_t connection_manager_stop(void)
{
    if (!s_monitoring) {
        return ESP_OK;
    }

    s_monitor_running = false;
    s_monitor_task = NULL;
    s_monitoring = false;

    ESP_LOGI(TAG, "Connection monitoring stopped");
    return ESP_OK;
}

esp_err_t connection_manager_register_handler(conn_mgr_event_t event, conn_mgr_cb_t cb)
{
    if (event >= CONN_MGR_EVENT_MAX || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callbacks[event] = cb;
    return ESP_OK;
}

conn_mgr_state_t connection_manager_get_state(conn_mgr_layer_t layer)
{
    if (layer >= CONN_MGR_LAYER_MAX) {
        return CONN_MGR_STATE_IDLE;
    }
    return s_stats.layer_state[layer];
}

conn_mgr_stats_t connection_manager_get_stats(void)
{
    return s_stats;
}

esp_err_t connection_manager_reconnect(conn_mgr_layer_t layer)
{
    if (layer >= CONN_MGR_LAYER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (layer) {
    case CONN_MGR_LAYER_WIFI:
        return wifi_manager_connect();
    case CONN_MGR_LAYER_WS_CLIENT:
        ESP_LOGI(TAG, "WS client reconnect triggered");
        return ESP_OK;
    case CONN_MGR_LAYER_WS_SERVER:
        return ws_manager_server_start();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

#endif /* CONNECTION_MANAGER_ENABLE */
