#ifndef WIFI_MANAGER_TYPES_H
#define WIFI_MANAGER_TYPES_H

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi Manager event types
 */
typedef enum {
    WIFI_MANAGER_EVENT_CONNECTED,       /*!< WiFi connected to AP */
    WIFI_MANAGER_EVENT_DISCONNECTED,    /*!< WiFi disconnected from AP */
    WIFI_MANAGER_EVENT_GOT_IP,          /*!< Got IP address */
    WIFI_MANAGER_EVENT_SCAN_DONE,       /*!< WiFi scan completed */
} wifi_manager_event_t;

/**
 * @brief WiFi Manager connection info
 */
typedef struct {
    char ssid[33];          /*!< Connected SSID */
    char ip[16];            /*!< IP address string */
    int8_t rssi;            /*!< Signal strength */
    uint8_t channel;        /*!< Channel number */
    bool is_connected;      /*!< Connection status */
} wifi_manager_info_t;

/**
 * @brief WiFi Manager AP scan result entry
 */
typedef struct {
    char ssid[33];          /*!< SSID of the AP */
    int8_t rssi;            /*!< Signal strength */
    uint8_t channel;        /*!< Primary channel */
    uint8_t authmode;       /*!< Authentication mode */
    uint8_t bssid[6];       /*!< BSSID of the AP */
} wifi_manager_ap_info_t;

/**
 * @brief WiFi Manager event callback type
 *
 * @param event  Event type
 * @param data   Event data (context-dependent, may be NULL)
 */
typedef void (*wifi_manager_cb_t)(wifi_manager_event_t event, void *data);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_TYPES_H */
