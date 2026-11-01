#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
void test_ws_critical_enter(void);
void test_ws_critical_exit(void);
#define portENTER_CRITICAL(lock) ((void)(lock), test_ws_critical_enter())
#define portEXIT_CRITICAL(lock) ((void)(lock), test_ws_critical_exit())
