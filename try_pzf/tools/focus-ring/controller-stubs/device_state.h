#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { APP_STATE_IDLE,APP_STATE_FOCUS,APP_STATE_COMPANION } app_state_t;
static inline esp_err_t device_state_init(void){return 0;}
static inline esp_err_t device_state_set_app_state(app_state_t s){(void)s;return 0;}
static inline esp_err_t device_state_set_voice_active(bool s){(void)s;return 0;}
