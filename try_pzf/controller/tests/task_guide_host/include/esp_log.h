#pragma once
void task_guide_host_log(const char *tag,const char *format,...);
#define ESP_LOGI(...) task_guide_host_log(__VA_ARGS__)
#define ESP_LOGW(...) task_guide_host_log(__VA_ARGS__)
