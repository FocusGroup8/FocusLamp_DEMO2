#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *SemaphoreHandle_t;
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define tskIDLE_PRIORITY 0
typedef struct {
    void *(*alloc)(size_t);
    void (*release)(void *);
    void *(*create_sem)(void);
    void (*delete_sem)(void *);
    int (*take)(void *,uint32_t);
    int (*give)(void *);
    void (*enter)(void *);
    void (*leave)(void *);
    int (*spawn)(void *,void *);
    void (*sleep_ms)(uint32_t);
    int64_t (*now_us)(void);
} probe_host_ops_t;
void probe_host_set_ops(const probe_host_ops_t *ops);
void probe_host_enter(void *lock);
void probe_host_leave(void *lock);
#define portENTER_CRITICAL(p) probe_host_enter(p)
#define portEXIT_CRITICAL(p) probe_host_leave(p)
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t,uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*entry)(void *),const char *,uint32_t,void *,int,void *);
void vTaskDelay(uint32_t);
void vTaskDelete(void *);
unsigned uxTaskGetStackHighWaterMark(void *);
int64_t esp_timer_get_time(void);
void probe_host_log(const char *tag,const char *format,...);
