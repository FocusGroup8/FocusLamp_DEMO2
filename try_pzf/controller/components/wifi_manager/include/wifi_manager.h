#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "wifi_manager_config.h"
#include "wifi_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (WIFI_MANAGER_ENABLE == 1)

/**
 * @brief Initialize WiFi Manager
 *
 * Performs: NVS init, netif init, event loop, ESP-Hosted SDIO transport, WiFi Remote STA.
 * After init, WiFi will attempt to connect to the configured AP.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to configured AP
 *
 * Starts WiFi connection. If already connected, returns ESP_OK.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_connect(void);

/**
 * @brief Disconnect from AP
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Deinitialize WiFi Manager
 *
 * Stops WiFi, releases resources. After deinit, wifi_manager_init() can be called again.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_deinit(void);

/**
 * @brief Check if WiFi is connected
 *
 * @return true if connected with IP, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Perform WiFi scan
 *
 * @param[out] aps    Array to store AP info records (caller-allocated)
 * @param[in,out] count  On input: max records to return. On output: actual count
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_scan(wifi_manager_ap_info_t *aps, uint16_t *count);

/**
 * @brief Get current connection info
 *
 * @param[out] info  Pointer to wifi_manager_info_t to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_get_info(wifi_manager_info_t *info);

/**
 * @brief Register event callback
 *
 * @param event  Event type to register for
 * @param cb     Callback function
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_manager_register_handler(wifi_manager_event_t event, wifi_manager_cb_t cb);

#else /* WIFI_MANAGER_ENABLE == 0 */

static inline esp_err_t wifi_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_manager_connect(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_manager_disconnect(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_manager_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool wifi_manager_is_connected(void) { return false; }
static inline esp_err_t wifi_manager_scan(wifi_manager_ap_info_t *aps, uint16_t *count) { (void)aps; (void)count; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_manager_get_info(wifi_manager_info_t *info) { (void)info; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_manager_register_handler(wifi_manager_event_t event, wifi_manager_cb_t cb) { (void)event; (void)cb; return ESP_ERR_NOT_SUPPORTED; }

#endif /* WIFI_MANAGER_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
