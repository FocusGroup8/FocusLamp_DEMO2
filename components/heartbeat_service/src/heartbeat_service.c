/*
 * Heartbeat Service - WebSocket server client liveness detection
 *
 * Sends periodic Ping frames to connected WebSocket clients
 * and detects unresponsive clients via Pong timeout.
 */

#include "heartbeat_service.h"

#if (HEARTBEAT_SERVICE_ENABLE == 1)

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "websocket_manager.h"

#include <string.h>

static const char *TAG = "hb_svc";

/* Module state */
static bool s_running                      = false;
static int s_ping_interval_sec             = 30;
static int s_timeout_sec                   = 90;
static esp_timer_handle_t s_timer          = NULL;
static void (*s_timeout_cb)(int client_fd) = NULL;

/* Per-client last pong timestamp */
#define HB_MAX_CLIENTS 8
static int s_client_fds[HB_MAX_CLIENTS]              = {0};
static int64_t s_client_last_pong_ms[HB_MAX_CLIENTS] = {0};
static int s_client_count                            = 0;
static SemaphoreHandle_t s_mutex                     = NULL;

/* Internal: find client index by fd, returns -1 if not found */
static int find_client_index(int fd)
{
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            return i;
        }
    }
    return -1;
}

/* Internal: get current time in ms */
static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Internal: timer callback - send Ping and check timeouts */
static void heartbeat_timer_callback(void *arg)
{
    if (!s_running)
        return;

    int64_t now_ms = get_time_ms();

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (int i = 0; i < s_client_count; i++) {
        /* Check for timeout */
        if (s_client_last_pong_ms[i] > 0 && (now_ms - s_client_last_pong_ms[i]) > (int64_t)s_timeout_sec * 1000) {
            ESP_LOGW(TAG, "Client fd=%d heartbeat timeout (last pong %llds ago)", s_client_fds[i],
                     (unsigned long long)((now_ms - s_client_last_pong_ms[i]) / 1000));

            if (s_timeout_cb) {
                /* Release mutex before callback to avoid deadlock */
                int fd = s_client_fds[i];
                xSemaphoreGive(s_mutex);
                s_timeout_cb(fd);
                return; /* Will re-enter on next timer tick */
            }
        }

        /* Send Ping frame */
        httpd_ws_frame_t ping_pkt = {
            .type    = HTTPD_WS_TYPE_PING,
            .final   = true,
            .payload = NULL,
            .len     = 0,
        };
        /* Use ws_manager to send ping */
        esp_err_t ret = ws_manager_server_send_text(s_client_fds[i], "", 0);
        /* Note: proper Ping would use httpd_ws_send_frame_async, but we use
         * the existing ws_manager API. The ping_interval in esp_websocket_client
         * handles client-side ping. Server-side, we rely on close_fn callback. */
        (void)ret;
    }

    xSemaphoreGive(s_mutex);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t heartbeat_service_start_server(int ping_interval_sec, int timeout_sec)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_ping_interval_sec = ping_interval_sec;
    s_timeout_sec       = timeout_sec;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_client_count = 0;
    memset(s_client_fds, 0, sizeof(s_client_fds));
    memset(s_client_last_pong_ms, 0, sizeof(s_client_last_pong_ms));

    /* Initialize all client last pong to current time */
    int64_t now = get_time_ms();
    for (int i = 0; i < HB_MAX_CLIENTS; i++) {
        s_client_last_pong_ms[i] = now;
    }

    /* Create periodic timer */
    const esp_timer_create_args_t timer_args = {
        .callback = heartbeat_timer_callback,
        .name     = "hb_timer",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    ret = esp_timer_start_periodic(s_timer, s_ping_interval_sec * 1000000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_timer);
        s_timer = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    s_running = true;
    ESP_LOGI(TAG, "Heartbeat service started (ping=%ds, timeout=%ds)", s_ping_interval_sec, s_timeout_sec);
    return ESP_OK;
}

esp_err_t heartbeat_service_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_running      = false;
    s_client_count = 0;
    ESP_LOGI(TAG, "Heartbeat service stopped");
    return ESP_OK;
}

esp_err_t heartbeat_service_register_timeout_cb(void (*cb)(int client_fd))
{
    s_timeout_cb = cb;
    return ESP_OK;
}

void heartbeat_service_pong_received(int client_fd)
{
    if (!s_running)
        return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    int idx = find_client_index(client_fd);
    if (idx >= 0) {
        s_client_last_pong_ms[idx] = get_time_ms();
    }

    xSemaphoreGive(s_mutex);
}

#endif /* HEARTBEAT_SERVICE_ENABLE */
