#ifndef WIFI_MANAGER_CONFIG_H
#define WIFI_MANAGER_CONFIG_H

#include "sdkconfig.h"

/* WiFi Manager component enable/disable switch */
#define WIFI_MANAGER_ENABLE CONFIG_WIFI_MANAGER_ENABLE

#if (WIFI_MANAGER_ENABLE == 1)

/* WiFi connection parameters (from menuconfig) */
#define WIFI_MANAGER_SSID       CONFIG_WIFI_MANAGER_SSID
#define WIFI_MANAGER_PASSWORD   CONFIG_WIFI_MANAGER_PASSWORD
#define WIFI_MANAGER_MAX_RETRY  CONFIG_WIFI_MANAGER_MAX_RETRY

/* Auto-reconnect configuration */
#define WIFI_MANAGER_AUTO_RECONNECT CONFIG_WIFI_MANAGER_AUTO_RECONNECT

#endif /* WIFI_MANAGER_ENABLE */

#endif /* WIFI_MANAGER_CONFIG_H */
