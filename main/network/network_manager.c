/*
 * network_manager.c - Network subsystem manager for FocusLamp (dowm)
 *
 * Mirrors the communication stack of Project 1 (FocusLamp_DEMO2):
 * WiFi -> WebSocket Server -> MCP Handler -> Connection Manager ->
 * Heartbeat Service -> Message Queue.
 *
 * See network_manager.h for the high-level sequence and the differences
 * from Project 1 (no camera, no REST registration, uses app_mcp_handler).
 */

#include "network_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Communication stack components */
#include "wifi_manager.h"
#include "websocket_manager.h"
#include "connection_manager.h"
#include "heartbeat_service.h"
#include "status_reporter.h"
#include "message_queue.h"

/* MCP handler (dowm's JSON-RPC 2.0 tool dispatcher over /mcp) */
#include "app_mcp_handler.h"

/* REST API for external HTTP control (Phase A: /api/status) */
#include "rest_api.h"

static const char *TAG = "net_mgr";

#define WIFI_CONNECT_TIMEOUT_MS 30000

static bool s_initialized = false;
static SemaphoreHandle_t s_wifi_connected_sem = NULL;
static char s_ip_string[16] = {0};

/*---------------------------------------------------------------
 * Internal: WiFi event callback
 *
 * Registered BEFORE wifi_manager_init() so the GOT_IP event is not
 * missed (wifi_manager_init may block until connection resolves).
 *-------------------------------------------------------------*/
static void wifi_event_handler(wifi_manager_event_t event, void *data)
{
    (void)data;
    switch (event) {
    case WIFI_MANAGER_EVENT_GOT_IP:
        ESP_LOGI(TAG, "WiFi got IP");
        if (s_wifi_connected_sem) {
            xSemaphoreGive(s_wifi_connected_sem);
        }
        break;
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi disconnected");
        break;
    default:
        break;
    }
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/

esp_err_t network_manager_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Initialize WiFi ---------------------------------------------- */
    ESP_LOGI(TAG, "Initializing WiFi...");
    s_wifi_connected_sem = xSemaphoreCreateBinary();
    if (!s_wifi_connected_sem) {
        return ESP_ERR_NO_MEM;
    }

    /* Register handlers BEFORE wifi_manager_init() because wifi_manager_init()
     * internally waits for the connection result (EventGroup bits). If we
     * register after init, the GOT_IP event will be missed and the semaphore
     * never given, causing a spurious 30s timeout. */
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_handler);

    esp_err_t ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
        return ret;
    }

    /* wifi_manager_init() may block until WiFi connects. The GOT_IP callback
     * registered above should already have fired and given the semaphore.
     * If already connected, ensure the semaphore is given (idempotent). */
    if (wifi_manager_is_connected()) {
        xSemaphoreGive(s_wifi_connected_sem);
    }

    /* Wait for IP with timeout (returns immediately if already connected) */
    ESP_LOGI(TAG, "Waiting for IP (timeout=%dms)...", WIFI_CONNECT_TIMEOUT_MS);
    if (xSemaphoreTake(s_wifi_connected_sem, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "WiFi connect timeout");
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
        return ESP_ERR_TIMEOUT;
    }

    /* Capture IP address for logging and external queries */
    wifi_manager_info_t info = {0};
    if (wifi_manager_get_info(&info) == ESP_OK) {
        strncpy(s_ip_string, info.ip, sizeof(s_ip_string) - 1);
        s_ip_string[sizeof(s_ip_string) - 1] = '\0';
        ESP_LOGI(TAG, "WiFi connected, IP: %s, SSID: %s, RSSI: %d",
                 s_ip_string, info.ssid, info.rssi);
    }

    /* 2. Initialize WebSocket server ---------------------------------- */
    ESP_LOGI(TAG, "Initializing WebSocket server...");
    ret = ws_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket server running at ws://%s:80/{ws,mcp}", s_ip_string);

    /* 3. Initialize REST API (registers /api/ HTTP endpoints) -------- */
    /* rest_api_init() registers REST URI handlers on the existing HTTP server
     * via ws_manager_server_register_uri(). Must run after ws_manager_server_start().
     * Non-fatal: if it fails, REST endpoints won't be available but WS still works. */
    ESP_LOGI(TAG, "Initializing REST API...");
    ret = rest_api_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "REST API init failed: %s (continuing)", esp_err_to_name(ret));
        /* Non-fatal: REST endpoints won't work, but WS server still runs */
    } else {
        ESP_LOGI(TAG, "REST API available at http://%s/api/status", s_ip_string);
    }

    /* 4. Initialize MCP handler (registers /mcp DATA handler) --------- */
    /* app_mcp_handler_init() calls ws_manager_register_handler() internally
     * to route /mcp text frames to the JSON-RPC 2.0 engine. Must run after
     * ws_manager_init() + ws_manager_server_start(). Also requires hardware
     * services (led_service, lcd_module) to be initialized. */
    ESP_LOGI(TAG, "Initializing MCP handler...");
    ret = app_mcp_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MCP handler init failed: %s (continuing)", esp_err_to_name(ret));
        /* Non-fatal: MCP over WS won't work, but WS server still runs */
    }

    /* 4. Initialize Connection Manager (WiFi + WebSocket health) ------ */
    ESP_LOGI(TAG, "Initializing connection manager...");
    conn_mgr_config_t conn_cfg = {
        .wifi_init_timeout_ms        = WIFI_CONNECT_TIMEOUT_MS,
        .wifi_reconnect_max_delay_ms = 60000,
        .ws_reconnect_max_delay_ms   = 60000,
        .monitor_interval_ms         = 5000,
    };
    ret = connection_manager_init(&conn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connection manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = connection_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connection manager start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. Initialize Heartbeat Service (WebSocket server liveness) ----- */
    ESP_LOGI(TAG, "Starting heartbeat service...");
    ret = heartbeat_service_start_server(30, 90);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Heartbeat service start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. Initialize Message Queue (reliable delivery) ----------------- */
    ESP_LOGI(TAG, "Initializing message queue...");
    ret = message_queue_init(16, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Message queue init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. Initialize Status Reporter (sends status to wifi_test) ------ */
    ESP_LOGI(TAG, "Initializing status reporter...");
    ret = status_reporter_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Status reporter init failed: %s (continuing)", esp_err_to_name(ret));
        /* Non-fatal: status reporting won't work, but network stack runs */
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Network manager initialized successfully");
    ESP_LOGI(TAG, "  General endpoint: ws://%s/ws", s_ip_string);
    ESP_LOGI(TAG, "  MCP control:      ws://%s/mcp", s_ip_string);
    ESP_LOGI(TAG, "  Root page:        http://%s/", s_ip_string);
    return ESP_OK;
}

void network_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    /* Stop services in reverse init order */
    status_reporter_deinit();
    message_queue_deinit();
    heartbeat_service_stop();
    connection_manager_stop();
    connection_manager_deinit();

    ws_manager_server_stop();
    ws_manager_deinit();

    wifi_manager_disconnect();
    wifi_manager_deinit();

    if (s_wifi_connected_sem) {
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
    }

    s_initialized = false;
    s_ip_string[0] = '\0';
    ESP_LOGI(TAG, "Network manager deinitialized");
}

const char *network_manager_get_ip(void)
{
    return s_ip_string;
}

bool network_manager_is_ready(void)
{
    return s_initialized;
}
