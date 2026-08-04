/*
 * app_event_handler.h - Global event handler registration for FocusLamp
 */

#pragma once
#ifndef __APP_EVENT_HANDLER_H__
#define __APP_EVENT_HANDLER_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and register all global event handlers.
 *        Handles system-level events such as:
 *        - EV_SYS_* system events
 *        - Power management events
 *        - Touch gesture events (default behavior)
 *        - Error events
 *        - Mode change coordination
 *        - Watchdog heartbeat monitoring
 */
esp_err_t app_event_handler_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_EVENT_HANDLER_H__ */