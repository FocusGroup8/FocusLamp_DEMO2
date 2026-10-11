#pragma once

#include "esp_err.h"

/* GET and POST /api/mode share the existing controller HTTP server.
 * This batch exposes state/countdown only; it does not drive peripherals. */
esp_err_t mode_http_api_register(void);
