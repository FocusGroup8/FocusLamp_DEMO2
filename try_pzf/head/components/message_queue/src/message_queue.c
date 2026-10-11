/*
 * Message Queue - Reliable message delivery for WebSocket server
 *
 * Queues messages that fail to send and retries them periodically.
 * Automatically clears messages for disconnected clients.
 */

#include "message_queue.h"

#if (MESSAGE_QUEUE_ENABLE == 1)

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "websocket_manager.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "msg_queue";

/* Pending message entry */
typedef struct {
    int client_fd;
    char *data;
    int len;
    msg_queue_type_t type;
    int retry_count;
    bool in_use;
} msg_entry_t;

/* Module state */
static msg_entry_t *s_entries    = NULL;
static int s_max_pending         = 16;
static int s_max_retries         = 3;
static int s_entry_count         = 0;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized        = false;

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t message_queue_init(int max_pending_msgs, int max_retries)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_max_pending = max_pending_msgs;
    s_max_retries = max_retries;

    s_entries = calloc(s_max_pending, sizeof(msg_entry_t));
    if (s_entries == NULL) {
        ESP_LOGE(TAG, "Failed to allocate queue entries");
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(s_entries);
        s_entries = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(s_entries, 0, s_max_pending * sizeof(msg_entry_t));
    s_entry_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Message queue initialized (max=%d, retries=%d)", s_max_pending, s_max_retries);
    return ESP_OK;
}

esp_err_t message_queue_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        /* Free all pending message data */
        for (int i = 0; i < s_max_pending; i++) {
            if (s_entries[i].in_use && s_entries[i].data) {
                free(s_entries[i].data);
            }
        }
        xSemaphoreGive(s_mutex);
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    free(s_entries);
    s_entries     = NULL;
    s_entry_count = 0;
    s_initialized = false;

    ESP_LOGI(TAG, "Message queue deinitialized");
    return ESP_OK;
}

esp_err_t message_queue_enqueue(int client_fd, const char *data, int len, msg_queue_type_t type)
{
    if (!s_initialized || data == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Try immediate send first */
    esp_err_t send_ret;
    if (type == MSG_QUEUE_TYPE_TEXT) {
        send_ret = ws_manager_server_send_text(client_fd, data, len);
    } else {
        send_ret = ws_manager_server_send_binary(client_fd, data, len);
    }

    if (send_ret == ESP_OK) {
        return ESP_OK; /* Sent successfully, no need to queue */
    }

    /* Send failed, queue for retry */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take mutex for enqueue");
        return ESP_ERR_TIMEOUT;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < s_max_pending; i++) {
        if (!s_entries[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGW(TAG, "Queue full, dropping message for fd=%d", client_fd);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Copy data */
    s_entries[slot].data = malloc(len);
    if (s_entries[slot].data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate message data (%d bytes)", len);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_entries[slot].data, data, len);
    s_entries[slot].client_fd   = client_fd;
    s_entries[slot].len         = len;
    s_entries[slot].type        = type;
    s_entries[slot].retry_count = 0;
    s_entries[slot].in_use      = true;
    s_entry_count++;

    ESP_LOGD(TAG, "Queued message for fd=%d (type=%s, len=%d, queue=%d/%d)", client_fd,
             type == MSG_QUEUE_TYPE_TEXT ? "TEXT" : "BIN", len, s_entry_count, s_max_pending);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t message_queue_process_pending(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_OK; /* Non-critical, try next time */
    }

    for (int i = 0; i < s_max_pending; i++) {
        if (!s_entries[i].in_use)
            continue;

        /* Attempt resend */
        esp_err_t send_ret;
        if (s_entries[i].type == MSG_QUEUE_TYPE_TEXT) {
            send_ret = ws_manager_server_send_text(s_entries[i].client_fd, s_entries[i].data, s_entries[i].len);
        } else {
            send_ret = ws_manager_server_send_binary(s_entries[i].client_fd, s_entries[i].data, s_entries[i].len);
        }

        if (send_ret == ESP_OK) {
            /* Sent successfully, free slot */
            free(s_entries[i].data);
            memset(&s_entries[i], 0, sizeof(msg_entry_t));
            s_entry_count--;
        } else {
            s_entries[i].retry_count++;
            if (s_entries[i].retry_count >= s_max_retries) {
                ESP_LOGW(TAG, "Dropping message for fd=%d after %d retries", s_entries[i].client_fd,
                         s_entries[i].retry_count);
                free(s_entries[i].data);
                memset(&s_entries[i], 0, sizeof(msg_entry_t));
                s_entry_count--;
            }
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t message_queue_clear_client(int client_fd)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < s_max_pending; i++) {
        if (s_entries[i].in_use && s_entries[i].client_fd == client_fd) {
            free(s_entries[i].data);
            memset(&s_entries[i], 0, sizeof(msg_entry_t));
            s_entry_count--;
        }
    }

    ESP_LOGD(TAG, "Cleared messages for fd=%d", client_fd);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

#endif /* MESSAGE_QUEUE_ENABLE */
