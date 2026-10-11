#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register GET /api/device/info after ws_manager_server_start(). */
esp_err_t device_info_api_register(void);

#ifdef __cplusplus
}
#endif
