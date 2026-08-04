/*
 * demo_fake_detection.h - Demo fake detection module
 *
 * Simulates phone/computer use detection and sedentary reminder
 * using periodic timers that trigger TTS playback via xiaozhi.
 */

#pragma once
#ifndef __DEMO_FAKE_DETECTION_H__
#define __DEMO_FAKE_DETECTION_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize demo fake detection module.
 *        Subscribes to EV_SYS_STARTUP_COMPLETE and starts periodic timers.
 * @return ESP_OK on success
 */
esp_err_t demo_fake_detection_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEMO_FAKE_DETECTION_H__ */
