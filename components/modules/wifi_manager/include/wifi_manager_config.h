#ifndef WIFI_MANAGER_CONFIG_H
#define WIFI_MANAGER_CONFIG_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi Manager 组件开关（ESP-Hosted + WiFi Remote，适用于 ESP32-P4+C5） */
#define WIFI_MANAGER_ENABLE CONFIG_WIFI_MANAGER_ENABLE

#if (WIFI_MANAGER_ENABLE == 1)

/* WiFi 连接参数（来自 menuconfig） */
#define WIFI_MANAGER_SSID       CONFIG_WIFI_MANAGER_SSID
#define WIFI_MANAGER_PASSWORD   CONFIG_WIFI_MANAGER_PASSWORD
#define WIFI_MANAGER_MAX_RETRY  CONFIG_WIFI_MANAGER_MAX_RETRY

/* 自动重连配置 */
#define WIFI_MANAGER_AUTO_RECONNECT CONFIG_WIFI_MANAGER_AUTO_RECONNECT

#endif /* WIFI_MANAGER_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_CONFIG_H */
