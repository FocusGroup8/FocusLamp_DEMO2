#pragma once
#include "esp_err.h"

/* Start only after LCD and modes init. This task is the sole frame writer. */
esp_err_t lcd_status_ui_start(void);
