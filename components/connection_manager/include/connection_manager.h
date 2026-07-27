#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include "connection_manager_config.h"
#include "connection_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (CONNECTION_MANAGER_ENABLE == 1)

esp_err_t connection_manager_init(const conn_mgr_config_t *config);
esp_err_t connection_manager_deinit(void);
esp_err_t connection_manager_start(void);
esp_err_t connection_manager_stop(void);
esp_err_t connection_manager_register_handler(conn_mgr_event_t event, conn_mgr_cb_t cb);
conn_mgr_state_t connection_manager_get_state(conn_mgr_layer_t layer);
conn_mgr_stats_t connection_manager_get_stats(void);
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
