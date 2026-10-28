#pragma once
void vision_host_log(const char *,const char *,...);
#define ESP_LOGI(tag,...) vision_host_log(tag,__VA_ARGS__)
