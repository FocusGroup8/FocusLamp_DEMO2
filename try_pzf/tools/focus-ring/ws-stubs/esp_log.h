#pragma once
#include <inttypes.h>
#include <stddef.h>
void test_ws_log(const char *tag, const char *format, ...);
size_t esp_get_free_heap_size(void);
#define ESP_LOGI(...) test_ws_log(__VA_ARGS__)
#define ESP_LOGW(...) test_ws_log(__VA_ARGS__)
#define ESP_LOGE(...) test_ws_log(__VA_ARGS__)
#define ESP_LOGD(...) test_ws_log(__VA_ARGS__)
