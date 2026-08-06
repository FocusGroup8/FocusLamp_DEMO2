/*
 * lamp_head_controller_config.h - Configuration for lamp head controller
 *
 * Defines the target IP of the lamp head (camera-fps) board, configured
 * via menuconfig. Independent from status_reporter's target (voice board).
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAMP_HEAD_TARGET_IP CONFIG_LAMP_HEAD_TARGET_IP

#ifdef __cplusplus
}
#endif
