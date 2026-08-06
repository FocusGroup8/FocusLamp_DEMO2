/*
 * WebSocket Manager - Modular WebSocket management for ESP32
 *
 * Client: Based on espressif/esp_websocket_client component
 * Server: Based on esp_http_server WebSocket support
 */

#include "websocket_manager.h"

#if (WS_MANAGER_ENABLE == 1)

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Callback storage */
static ws_manager_cb_t s_callbacks[WS_MANAGER_EVENT_CLIENT_TIMEOUT + 1] = {NULL};

/* TAG used by both client and server code */
static const char *TAG = "ws_mgr";

/*---------------------------------------------------------------
 * Async WS send completion callback & frame buffer pool
 *-------------------------------------------------------------*/
#if WS_MANAGER_SERVER_ENABLE

#define WS_FRAME_POOL_SIZE 4          /* Number of pre-allocated frame buffers (2 clients × 2 in-flight) */
#define WS_FRAME_BUF_SIZE (32 * 1024) /* 32KB per buffer, sufficient for Q=15 YUV420 @ 800x640 */

/* EWMA smoothing factor for send duration (α=0.2, ~5 samples time constant) */
#define WS_SEND_DURATION_EWMA_ALPHA 0.2f

typedef struct {
    uint8_t data[WS_FRAME_BUF_SIZE];
    int len;
    atomic_bool in_use;
    int64_t start_time_us; /* Timestamp when send was initiated (for duration tracking) */
} ws_frame_buf_t;

static ws_frame_buf_t s_frame_pool[WS_FRAME_POOL_SIZE];

/* Send statistics: updated by httpd worker thread (ws_async_send_complete_cb)
 * and read by cam_stream task. Uses atomic operations for thread safety. */
static struct {
    uint32_t total_sent;           /* Total frames successfully sent */
    uint32_t total_failed;         /* Total frames failed to send */
    uint32_t consecutive_failures; /* Current consecutive failure count */
    uint32_t pool_exhausted;       /* Frame buffer pool exhausted count */
    int last_error;                /* Last error code (errno value) */
    int64_t last_send_duration_us; /* Last send duration (queue to completion) */
    int64_t avg_send_duration_us;  /* EWMA of send duration */
} s_send_stats;

/* Called by httpd worker thread after httpd_ws_send_data_async completes.
 * Returns the frame buffer to the pool.
 * On send failure, log a warning to help diagnose connection issues.
 * TCP keepalive (idle=3s, interval=2s, count=2) ensures broken
 * connections are detected within ~7 seconds, at which point httpd
 * calls ws_session_close_cb to clean up s_camera_client_fd. */
static void ws_async_send_complete_cb(esp_err_t err, int socket, void *arg)
{
    ws_frame_buf_t *buf = (ws_frame_buf_t *)arg;

    /* Calculate send duration (from queue to completion) */
    int64_t now_us      = esp_timer_get_time();
    int64_t duration_us = now_us - buf->start_time_us;

    if (err != ESP_OK) {
        /* Map ESP error codes to errno-like values for congestion classification:
         * - ESP_ERR_TIMEOUT / ESP_FAIL with EAGAIN: transient congestion (buffer full)
         * - ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_ARG: connection broken
         * The actual errno is logged but not directly accessible here;
         * we use the ESP error code as a proxy. */
        s_send_stats.total_failed++;
        s_send_stats.consecutive_failures++;
        s_send_stats.last_error = (int)err;

        /* Classify error for logging: transient vs fatal */
        if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL) {
            /* EAGAIN (errno 11) equivalent: transient congestion, buffer full */
            ESP_LOGD(TAG, "Transient send failure on fd=%d: %s (congestion)", socket, esp_err_to_name(err));
        } else {
            /* ECONNRESET (errno 104) or other: connection-level error */
            ESP_LOGW(TAG, "Async WS send failed on fd=%d: %s", socket, esp_err_to_name(err));
        }
    } else {
        s_send_stats.total_sent++;
        s_send_stats.consecutive_failures = 0;
        s_send_stats.last_error           = 0;

        /* Update send duration statistics (EWMA) */
        s_send_stats.last_send_duration_us = duration_us;
        if (s_send_stats.avg_send_duration_us == 0) {
            s_send_stats.avg_send_duration_us = duration_us;
        } else {
            float new_avg = WS_SEND_DURATION_EWMA_ALPHA * (float)duration_us +
                            (1.0f - WS_SEND_DURATION_EWMA_ALPHA) * (float)s_send_stats.avg_send_duration_us;
            s_send_stats.avg_send_duration_us = (int64_t)new_avg;
        }
    }

    atomic_store(&buf->in_use, false); /* Return buffer to pool */
}

/* Acquire a free buffer from the pool. Returns NULL if all in use. */
static ws_frame_buf_t *ws_frame_pool_acquire(void)
{
    for (int i = 0; i < WS_FRAME_POOL_SIZE; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&s_frame_pool[i].in_use, &expected, true)) {
            s_frame_pool[i].start_time_us = esp_timer_get_time();
            return &s_frame_pool[i];
        }
    }
    s_send_stats.pool_exhausted++;
    return NULL; /* All buffers busy — drop this frame */
}
#endif

static void dispatch_event(ws_manager_event_t event, void *data)
{
    if (event <= WS_MANAGER_EVENT_CLIENT_TIMEOUT && s_callbacks[event]) {
        s_callbacks[event](event, data);
    }
}

/* ======================== Client Implementation ======================== */

#if (WS_MANAGER_CLIENT_ENABLE == 1)

#include "esp_websocket_client.h"

static esp_websocket_client_handle_t s_ws_client = NULL;
static volatile bool s_client_connected          = false;
static SemaphoreHandle_t s_client_sem            = NULL;

/* Exponential backoff reconnect state */
static int s_reconnect_delay_ms     = 0;
static const int s_reconnect_min_ms = WS_MANAGER_RECONNECT_MS;
static int s_consecutive_errors     = 0;
#define WS_MANAGER_MAX_ERROR_LOGS 3 /* Only log first N consecutive errors */

static void websocket_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket client connected");
        s_client_connected   = true;
        s_reconnect_delay_ms = s_reconnect_min_ms;
        s_consecutive_errors = 0;
        dispatch_event(WS_MANAGER_EVENT_CONNECTED, NULL);
        if (s_client_sem) {
            xSemaphoreGive(s_client_sem);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_consecutive_errors <= WS_MANAGER_MAX_ERROR_LOGS) {
            ESP_LOGW(TAG, "WebSocket client disconnected");
        } else if (s_consecutive_errors % 10 == 0) {
            ESP_LOGW(TAG, "WebSocket client disconnected (%d consecutive errors)", s_consecutive_errors);
        }
        s_client_connected = false;
        dispatch_event(WS_MANAGER_EVENT_DISCONNECTED, NULL);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ws_data->op_code == 0x01 || ws_data->op_code == 0x02) {
            ws_manager_data_t msg = {
                .type      = (ws_data->op_code == 0x01) ? WS_DATA_TYPE_TEXT : WS_DATA_TYPE_BINARY,
                .data      = ws_data->data_ptr,
                .data_len  = ws_data->data_len,
                .client_fd = -1,
                .uri       = NULL,
            };
            dispatch_event(WS_MANAGER_EVENT_DATA, &msg);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        s_consecutive_errors++;
        if (s_consecutive_errors <= WS_MANAGER_MAX_ERROR_LOGS) {
            ESP_LOGE(TAG, "WebSocket client error");
        } else if (s_consecutive_errors % 10 == 0) {
            ESP_LOGW(TAG, "WebSocket client error (%d consecutive)", s_consecutive_errors);
        }
        dispatch_event(WS_MANAGER_EVENT_ERROR, NULL);
        break;

    default:
        break;
    }
}

esp_err_t ws_manager_client_start(const ws_manager_client_config_t *config)
{
    if (config == NULL || config->uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ws_client != NULL) {
        ESP_LOGW(TAG, "Client already started");
        return ESP_ERR_INVALID_STATE;
    }

    s_client_sem = xSemaphoreCreateBinary();
    if (s_client_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Configure WebSocket client */
    esp_websocket_client_config_t ws_cfg = {
        .uri                    = config->uri,
        .buffer_size            = WS_MANAGER_BUFFER_SIZE,
        .ping_interval_sec      = WS_MANAGER_PING_INTERVAL,
        .reconnect_timeout_ms   = WS_MANAGER_RECONNECT_MS,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
    };

    if (config->subprotocol) {
        ws_cfg.subprotocol = config->subprotocol;
    }
    if (config->user_agent) {
        ws_cfg.user_agent = config->user_agent;
    }
    if (config->headers) {
        ws_cfg.headers = config->headers;
    }
#if (WS_MANAGER_TLS_ENABLE == 1)
    if (config->cert_pem) {
        ws_cfg.cert_pem = config->cert_pem;
    }
#else
    ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;
#endif

    ESP_LOGI(TAG, "Starting WebSocket client, URI: %s", config->uri);

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        vSemaphoreDelete(s_client_sem);
        s_client_sem = NULL;
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        vSemaphoreDelete(s_client_sem);
        s_client_sem = NULL;
        return err;
    }

    /* Wait for connection with timeout */
    if (xSemaphoreTake(s_client_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timeout waiting for WebSocket connection");
        /* Don't return error - client is started, will auto-reconnect */
    }

    return ESP_OK;
}

esp_err_t ws_manager_client_stop(void)
{
    if (s_ws_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping WebSocket client");
    esp_err_t err = esp_websocket_client_close(s_ws_client, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Close failed: %s, destroying anyway", esp_err_to_name(err));
    }

    esp_websocket_client_destroy(s_ws_client);
    s_ws_client        = NULL;
    s_client_connected = false;

    if (s_client_sem) {
        vSemaphoreDelete(s_client_sem);
        s_client_sem = NULL;
    }

    return ESP_OK;
}

bool ws_manager_client_is_connected(void)
{
    return s_ws_client != NULL && s_client_connected;
}

int ws_manager_client_send_text(const char *data, int len, int timeout_ms)
{
    if (s_ws_client == NULL || !s_client_connected) {
        return -1;
    }
    return esp_websocket_client_send_text(s_ws_client, data, len, timeout_ms);
}

int ws_manager_client_send_binary(const char *data, int len, int timeout_ms)
{
    if (s_ws_client == NULL || !s_client_connected) {
        return -1;
    }
    return esp_websocket_client_send_bin(s_ws_client, data, len, timeout_ms);
}

esp_err_t ws_manager_client_get_info(ws_manager_conn_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->is_connected = s_client_connected;
    info->fd           = -1;
    return ESP_OK;
}

#endif /* WS_MANAGER_CLIENT_ENABLE */

/* ======================== Server Implementation ======================== */

#if (WS_MANAGER_SERVER_ENABLE == 1)

#include "esp_http_server.h"

static httpd_handle_t s_server = NULL;
static bool s_server_running   = false;

/* Connected client tracking */
static int s_client_fds[WS_MANAGER_SERVER_MAX_CONN];
static int s_client_count               = 0;
static SemaphoreHandle_t s_server_mutex = NULL;

/* Camera stream client limit: only 1 concurrent /camera connection allowed.
 * Camera streaming is bandwidth-intensive; multiple clients would exceed
 * WiFi capacity and cause frame congestion / disconnection oscillation. */
#define WS_CAMERA_MAX_CLIENTS 1
static int s_camera_client_fd = -1;

/* Pre-allocated receive buffer to avoid frequent malloc/free */
static uint8_t s_recv_buf[WS_MANAGER_BUFFER_SIZE];

/* Memory water level: reject new connections below this threshold */
#define WS_MANAGER_MEM_WATERMARK (32 * 1024) /* 32KB */

/* Maximum frame size: reject frames larger than this */
#define WS_MANAGER_MAX_FRAME_SIZE (16 * 1024) /* 16KB */

static void track_client_add(int fd)
{
    if (s_server_mutex) {
        xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    }
    /* Memory water level check */
    if (esp_get_free_heap_size() < WS_MANAGER_MEM_WATERMARK) {
        ESP_LOGW(TAG, "Server: low memory (%" PRIu32 " bytes), rejecting client fd=%d", esp_get_free_heap_size(), fd);
        if (s_server_mutex) {
            xSemaphoreGive(s_server_mutex);
        }
        return;
    }
    if (s_client_count < WS_MANAGER_SERVER_MAX_CONN) {
        s_client_fds[s_client_count++] = fd;
    }
    if (s_server_mutex) {
        xSemaphoreGive(s_server_mutex);
    }
}

static void track_client_remove(int fd)
{
    if (s_server_mutex) {
        xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    }
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            s_client_fds[i] = s_client_fds[s_client_count - 1];
            s_client_count--;
            dispatch_event(WS_MANAGER_EVENT_SERVER_DISCONNECT, NULL);
            break;
        }
    }
    /* Also clear camera client tracking */
    if (s_camera_client_fd == fd) {
        s_camera_client_fd = -1;
        ESP_LOGI(TAG, "Server: /camera client slot released (fd=%d)", fd);
    }
    if (s_server_mutex) {
        xSemaphoreGive(s_server_mutex);
    }
}

/* Session close callback: detect client disconnection */
static void ws_session_close_cb(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Server: client disconnected, fd=%d", sockfd);
    track_client_remove(sockfd);
    close(sockfd);
}

/* Session open callback: optimize socket for WebSocket streaming */
static esp_err_t ws_session_open_cb(httpd_handle_t hd, int sockfd)
{
    /* Set TCP_NODELAY: disable Nagle's algorithm for lower latency.
     * Critical for WebSocket streaming where small header frames
     * should be sent immediately without waiting for more data. */
    int nodelay = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_NODELAY on fd=%d (errno=%d)", sockfd, errno);
    }

    /* Override SO_SNDTIMEO to 500ms (from httpd default of 1s).
     * When the TCP send buffer is full, send() blocks for SO_SNDTIMEO
     * before returning EAGAIN. The httpd worker thread is single-threaded,
     * so a very long block stalls ALL WebSocket processing.
     * 500ms provides enough time for the TCP buffer to drain on slow
     * WiFi while keeping the httpd thread responsive. The previous 100ms
     * was too aggressive and caused frequent send timeouts on weak networks. */
    struct timeval tv = {
        .tv_sec  = 0,
        .tv_usec = 500000, /* 500ms */
    };
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_SNDTIMEO on fd=%d (errno=%d)", sockfd, errno);
    }

    /* Enable TCP keepalive to detect dead connections quickly.
     * Without keepalive, a half-open connection (e.g., client lost WiFi)
     * can persist for 30+ seconds before TCP detects it. During this time,
     * s_camera_client_fd still holds the old fd, and new connections are
     * rejected with 403, making auto-reconnect appear broken.
     * With keepalive idle=3s, interval=2s, count=2, a dead connection
     * is detected in ~7 seconds. */
    int keepalive = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_KEEPALIVE on fd=%d (errno=%d)", sockfd, errno);
    }
    int idle = 3; /* Start probing after 3 seconds of idle */
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPIDLE on fd=%d (errno=%d)", sockfd, errno);
    }
    int interval = 2; /* Send probe every 2 seconds */
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPINTVL on fd=%d (errno=%d)", sockfd, errno);
    }
    int count = 2; /* Close after 2 failed probes */
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPCNT on fd=%d (errno=%d)", sockfd, errno);
    }

    return ESP_OK;
}

static esp_err_t ws_server_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake complete - new client connected */
        int fd = httpd_req_to_sockfd(req);

        /* Camera stream client limit: reject if /camera already occupied */
        if (strcmp(req->uri, "/camera") == 0) {
            if (s_camera_client_fd >= 0) {
                ESP_LOGW(TAG, "Server: rejecting /camera client fd=%d (already occupied by fd=%d)", fd,
                         s_camera_client_fd);
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Camera stream already in use");
                return ESP_FAIL;
            }
            s_camera_client_fd = fd;
            ESP_LOGI(TAG, "Server: new /camera client connected, fd=%d", fd);
        } else {
            ESP_LOGI(TAG, "Server: new client connected, fd=%d", fd);
        }

        track_client_add(fd);
        dispatch_event(WS_MANAGER_EVENT_SERVER_CONNECT, NULL);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* First call to get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Server: recv frame len failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len > 0) {
        /* Frame size limit check */
        if (ws_pkt.len > WS_MANAGER_MAX_FRAME_SIZE) {
            ESP_LOGE(TAG, "Server: frame too large (%d bytes, max=%d)", ws_pkt.len, WS_MANAGER_MAX_FRAME_SIZE);
            return ESP_ERR_NO_MEM;
        }

        /* Use pre-allocated buffer if fits, otherwise allocate */
        uint8_t *buf;
        bool dynamic_alloc = false;

        if (ws_pkt.len <= sizeof(s_recv_buf)) {
            buf = s_recv_buf;
        } else {
            buf = calloc(1, ws_pkt.len + 1);
            if (buf == NULL) {
                ESP_LOGE(TAG, "Server: no memory for recv buffer (len=%d)", ws_pkt.len);
                return ESP_ERR_NO_MEM;
            }
            dynamic_alloc = true;
        }
        ws_pkt.payload = buf;
        ret            = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Server: recv frame data failed: %s", esp_err_to_name(ret));
            if (dynamic_alloc) {
                free(buf);
            }
            return ret;
        }

        /* Dispatch data event */
        ws_manager_data_t msg = {
            .type      = (ws_pkt.type == HTTPD_WS_TYPE_TEXT) ? WS_DATA_TYPE_TEXT : WS_DATA_TYPE_BINARY,
            .data      = (const char *)ws_pkt.payload,
            .data_len  = ws_pkt.len,
            .client_fd = httpd_req_to_sockfd(req),
            .uri       = req->uri,
        };
        dispatch_event(WS_MANAGER_EVENT_DATA, &msg);

#if (WS_MANAGER_SERVER_ECHO == 1)
        /* Echo received message back to sender */
        httpd_ws_frame_t echo_pkt = {
            .payload = ws_pkt.payload,
            .len     = ws_pkt.len,
            .type    = ws_pkt.type,
            .final   = true,
        };
        httpd_ws_send_frame(req, &echo_pkt);
#endif

        if (dynamic_alloc) {
            free(buf);
        }
    }
    return ESP_OK;
}

static httpd_uri_t ws_uri = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_server_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

/* Camera stream endpoint: binary JPEG frames */
static httpd_uri_t ws_uri_camera = {
    .uri          = "/camera",
    .method       = HTTP_GET,
    .handler      = ws_server_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

/* MCP control endpoint: JSON-RPC 2.0 text frames */
static httpd_uri_t ws_uri_mcp = {
    .uri          = "/mcp",
    .method       = HTTP_GET,
    .handler      = ws_server_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

/* Root page handler: show device info and WebSocket endpoint */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html = "<!DOCTYPE html><html><head><title>ESP32 WebSocket</title></head>"
                       "<body><h1>ESP32-P4 WebSocket Server</h1>"
                       "<p>Camera stream: <code>ws://[device-ip]/camera</code></p>"
                       "<p>MCP control: <code>ws://[device-ip]/mcp</code></p>"
                       "<p>Legacy endpoint: <code>ws://[device-ip]/ws</code></p>"
                       "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html);
}

static httpd_uri_t root_uri = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_handler,
    .user_ctx = NULL,
};

esp_err_t ws_manager_server_start(void)
{
    if (s_server_running) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_server_mutex = xSemaphoreCreateMutex();
    if (s_server_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size     = 16384; /* Increased from default 4096 to accommodate 32KB TCP send buffer */
    config.send_wait_timeout =
        1; /* Reduced from default 5s: fail fast on slow networks instead of blocking httpd worker */
    config.server_port = WS_MANAGER_SERVER_PORT;
    config.max_uri_handlers =
        28; /* 4(WebSocket URIs) + 18(REST API: camera 4 + display 4 + led 5 + status 1 + eyes 4) + 3(touch) + 3(reserved) */
    config.max_open_sockets = WS_MANAGER_SERVER_MAX_CONN + 2; /* Reserve for HTTP + control */
    config.close_fn         = ws_session_close_cb;
    config.open_fn          = ws_session_open_cb;

    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_server_mutex);
        s_server_mutex = NULL;
        return err;
    }

    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &ws_uri_camera);
    httpd_register_uri_handler(s_server, &ws_uri_mcp);
    httpd_register_uri_handler(s_server, &root_uri);
    s_server_running = true;
    s_client_count   = 0;
    memset(s_client_fds, 0, sizeof(s_client_fds));

    ESP_LOGI(TAG, "WebSocket server started");
    return ESP_OK;
}

esp_err_t ws_manager_server_stop(void)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping WebSocket server");
    esp_err_t err      = httpd_stop(s_server);
    s_server           = NULL;
    s_server_running   = false;
    s_client_count     = 0;
    s_camera_client_fd = -1;

    if (s_server_mutex) {
        vSemaphoreDelete(s_server_mutex);
        s_server_mutex = NULL;
    }

    return err;
}

bool ws_manager_server_is_running(void)
{
    return s_server_running;
}

esp_err_t ws_manager_server_register_uri(const httpd_uri_t *uri)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!uri) {
        return ESP_ERR_INVALID_ARG;
    }
    return httpd_register_uri_handler(s_server, uri);
}

esp_err_t ws_manager_server_send_text(int client_fd, const char *data, int len)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t ws_pkt = {
        .payload = (uint8_t *)data,
        .len     = len,
        .type    = HTTPD_WS_TYPE_TEXT,
        .final   = true,
    };
    return httpd_ws_send_frame_async(s_server, client_fd, &ws_pkt);
}

esp_err_t ws_manager_server_send_binary(int client_fd, const char *data, int len)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len > WS_FRAME_BUF_SIZE) {
        ESP_LOGW(TAG, "Frame too large (%d > %d), dropping", len, WS_FRAME_BUF_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Acquire a buffer from the pre-allocated pool (no malloc per frame) */
    ws_frame_buf_t *buf = ws_frame_pool_acquire();
    if (buf == NULL) {
        /* All pool buffers are in-flight — drop this frame (backpressure) */
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf->data, data, len);
    buf->len = len;

    httpd_ws_frame_t ws_pkt = {
        .payload = buf->data,
        .len     = len,
        .type    = HTTPD_WS_TYPE_BINARY,
        .final   = true,
    };

    /* Use httpd_ws_send_data_async (true async via httpd_queue_work)
     * instead of httpd_ws_send_frame_async (blocking send_fn call).
     * Completion callback returns the buffer to the pool. */
    esp_err_t err = httpd_ws_send_data_async(s_server, client_fd, &ws_pkt, ws_async_send_complete_cb, buf);
    if (err != ESP_OK) {
        atomic_store(&buf->in_use, false); /* Return buffer on queue failure */
    }
    return err;
}

esp_err_t ws_manager_server_broadcast_text(const char *data, int len)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (s_server_mutex) {
        xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    }
    for (int i = 0; i < s_client_count; i++) {
        esp_err_t err = ws_manager_server_send_text(s_client_fds[i], data, len);
        if (err != ESP_OK) {
            ret = err;
        }
    }
    if (s_server_mutex) {
        xSemaphoreGive(s_server_mutex);
    }
    return ret;
}

esp_err_t ws_manager_server_broadcast_binary(const char *data, int len)
{
    if (!s_server_running || s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Only send to the /camera client — binary JPEG frames are only
     * meaningful for camera streaming. Sending to /mcp or /ws clients
     * wastes bandwidth and causes them to disconnect on unexpected frames. */
    if (s_camera_client_fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_OK;
    if (s_server_mutex) {
        xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    }
    ret = ws_manager_server_send_binary(s_camera_client_fd, data, len);
    if (s_server_mutex) {
        xSemaphoreGive(s_server_mutex);
    }
    return ret;
}

int ws_manager_server_get_client_count(void)
{
    return s_client_count;
}

esp_err_t ws_manager_server_get_send_stats(ws_send_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_server_running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Copy statistics atomically. Individual fields are read non-atomically,
     * but the values are monotonic counters or approximately stable EWMA values,
     * so brief inconsistency between fields is acceptable for congestion detection. */
    stats->total_sent            = s_send_stats.total_sent;
    stats->total_failed          = s_send_stats.total_failed;
    stats->consecutive_failures  = s_send_stats.consecutive_failures;
    stats->pool_exhausted        = s_send_stats.pool_exhausted;
    stats->last_error            = s_send_stats.last_error;
    stats->last_send_duration_us = s_send_stats.last_send_duration_us;
    stats->avg_send_duration_us  = s_send_stats.avg_send_duration_us;
    return ESP_OK;
}

void ws_manager_server_reset_send_stats(void)
{
    s_send_stats.total_sent            = 0;
    s_send_stats.total_failed          = 0;
    s_send_stats.consecutive_failures  = 0;
    s_send_stats.pool_exhausted        = 0;
    s_send_stats.last_error            = 0;
    s_send_stats.last_send_duration_us = 0;
    s_send_stats.avg_send_duration_us  = 0;
}

#endif /* WS_MANAGER_SERVER_ENABLE */

/* ======================== Common API Implementation ======================== */

esp_err_t ws_manager_init(void)
{
    ESP_LOGI(TAG, "WebSocket Manager initialized");
    return ESP_OK;
}

esp_err_t ws_manager_deinit(void)
{
#if (WS_MANAGER_CLIENT_ENABLE == 1)
    if (s_ws_client) {
        ws_manager_client_stop();
    }
#endif

#if (WS_MANAGER_SERVER_ENABLE == 1)
    if (s_server_running) {
        ws_manager_server_stop();
    }
#endif

    memset(s_callbacks, 0, sizeof(s_callbacks));
    ESP_LOGI(TAG, "WebSocket Manager deinitialized");
    return ESP_OK;
}

esp_err_t ws_manager_register_handler(ws_manager_event_t event, ws_manager_cb_t cb)
{
    if (event > WS_MANAGER_EVENT_CLIENT_TIMEOUT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callbacks[event] = cb;
    return ESP_OK;
}

#endif /* WS_MANAGER_ENABLE */
