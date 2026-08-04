#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include "esp_err.h"
#include "message_queue_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (MESSAGE_QUEUE_ENABLE == 1)

/**
 * @brief Message types for the queue
 */
typedef enum {
    MSG_QUEUE_TYPE_TEXT,   /*!< Text message */
    MSG_QUEUE_TYPE_BINARY, /*!< Binary message */
} msg_queue_type_t;

/**
 * @brief Initialize message queue
 *
 * @param max_pending_msgs  Maximum pending messages (default 16)
 * @param max_retries       Maximum send retries per message (default 3)
 * @return ESP_OK on success
 */
esp_err_t message_queue_init(int max_pending_msgs, int max_retries);

/**
 * @brief Deinitialize message queue
 *
 * @return ESP_OK on success
 */
esp_err_t message_queue_deinit(void);

/**
 * @brief Enqueue a message for reliable delivery
 *
 * If the initial send fails, the message is queued for retry.
 *
 * @param client_fd  Target client fd
 * @param data       Message data (copied internally)
 * @param len        Data length
 * @param type       Message type
 * @return ESP_OK on success (either sent or queued)
 */
esp_err_t message_queue_enqueue(int client_fd, const char *data, int len, msg_queue_type_t type);

/**
 * @brief Process pending messages (retry failed sends)
 *
 * Should be called periodically, e.g., from connection_manager monitor task.
 *
 * @return ESP_OK on success
 */
esp_err_t message_queue_process_pending(void);

/**
 * @brief Clear all messages for a specific client
 *
 * Called when a client disconnects.
 *
 * @param client_fd  Client fd to clear
 * @return ESP_OK on success
 */
esp_err_t message_queue_clear_client(int client_fd);

#else /* MESSAGE_QUEUE_ENABLE == 0 */

static inline esp_err_t message_queue_init(int max_pending_msgs, int max_retries)
{
    (void)max_pending_msgs;
    (void)max_retries;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t message_queue_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t message_queue_enqueue(int client_fd, const char *data, int len, msg_queue_type_t type)
{
    (void)client_fd;
    (void)data;
    (void)len;
    (void)type;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t message_queue_process_pending(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t message_queue_clear_client(int client_fd)
{
    (void)client_fd;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* MESSAGE_QUEUE_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_QUEUE_H */
