/*
 * app_init.h - System initialization interface for FocusLamp
 */

#pragma once
#ifndef __APP_INIT_H__
#define __APP_INIT_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all system components in order:
 *        1. NVS flash
 *        2. Default event loop
 *        3. Network interface (optional)
 *        4. BSP (Board Support Package)
 *        5. Event bus
 *        6. Drivers
 *        7. Services
 */
void app_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_INIT_H__ */