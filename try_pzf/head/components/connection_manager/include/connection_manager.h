#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include "connection_manager_config.h"
#include "connection_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (CONNECTION_MANAGER_ENABLE == 1)

/**
 * @brief Initialize Connection Manager
 *
 * Registers event handlers for WiFi and WebSocket managers,
 * starts health monitoring task.
 *
 * @param config  Configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t connection_manager_init(const conn_mgr_config_t *config);

/**
 * @brief Deinitialize Connection Manager
 *
 * Stops monitoring, unregisters handlers.
 *
 * @return ESP_OK on success
 */
esp_err_t connection_manager_deinit(void);

/**
 * @brief Start connection monitoring
 *
 * Begins periodic health checks and enables automatic recovery.
 *
 * @return ESP_OK on success
 */
esp_err_t connection_manager_start(void);

/**
 * @brief Stop connection monitoring
 *
 * @return ESP_OK on success
 */
esp_err_t connection_manager_stop(void);

/**
 * @brief Register event callback
 *
 * @param event  Event type
 * @param cb     Callback function
 * @return ESP_OK on success
 */
esp_err_t connection_manager_register_handler(conn_mgr_event_t event, conn_mgr_cb_t cb);

/**
 * @brief Get state for a specific layer
 *
 * @param layer  Layer identifier
 * @return Current state
 */
conn_mgr_state_t connection_manager_get_state(conn_mgr_layer_t layer);

/**
 * @brief Get connection statistics
 *
 * @return Statistics structure
 */
conn_mgr_stats_t connection_manager_get_stats(void);

/**
 * @brief Manually trigger reconnection for a layer
 *
 * @param layer  Layer to reconnect
 * @return ESP_OK on success
 */
esp_err_t connection_manager_reconnect(conn_mgr_layer_t layer);

#else /* CONNECTION_MANAGER_ENABLE == 0 */

static inline esp_err_t connection_manager_init(const conn_mgr_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t connection_manager_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t connection_manager_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t connection_manager_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t connection_manager_register_handler(conn_mgr_event_t event, conn_mgr_cb_t cb)
{
    (void)event;
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline conn_mgr_state_t connection_manager_get_state(conn_mgr_layer_t layer)
{
    (void)layer;
    return CONN_MGR_STATE_IDLE;
}
static inline conn_mgr_stats_t connection_manager_get_stats(void)
{
    conn_mgr_stats_t stats = {0};
    return stats;
}
static inline esp_err_t connection_manager_reconnect(conn_mgr_layer_t layer)
{
    (void)layer;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONNECTION_MANAGER_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_MANAGER_H */
