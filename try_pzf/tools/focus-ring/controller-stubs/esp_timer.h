#pragma once
#include <stdint.h>
extern int64_t fake_now;
typedef void* esp_timer_handle_t;
typedef struct {void(*callback)(void*);const char*name;int dispatch_method;} esp_timer_create_args_t;
#define ESP_TIMER_TASK 0
static inline int64_t esp_timer_get_time(void){return fake_now;}
static inline int esp_timer_create(const esp_timer_create_args_t*a,esp_timer_handle_t*b){(void)a;*b=(void*)1;return 0;}
static inline int esp_timer_start_periodic(esp_timer_handle_t a,int64_t b){(void)a;(void)b;return 0;}
static inline int esp_timer_delete(esp_timer_handle_t a){(void)a;return 0;}
