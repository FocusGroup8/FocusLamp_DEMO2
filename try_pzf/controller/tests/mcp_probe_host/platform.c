#include "probe_host.h"
#include "string.h"
static probe_host_ops_t host;
void probe_host_set_ops(const probe_host_ops_t *ops) { host=*ops; }
void *malloc(size_t n) { return host.alloc(n); }
void free(void *p) { if(p) host.release(p); }
size_t strlen(const char *p) { size_t n=0; while(p[n])++n; return n; }
int strcmp(const char *a,const char *b) { while(*a&&*a==*b){++a;++b;} return (unsigned char)*a-(unsigned char)*b; }
void *memcpy(void *to,const void *from,size_t n) { unsigned char *d=to;const unsigned char *s=from;for(size_t i=0;i<n;++i)d[i]=s[i];return to; }
void *memset(void *to,int v,size_t n) { unsigned char *d=to;for(size_t i=0;i<n;++i)d[i]=(unsigned char)v;return to; }
void probe_host_enter(void *p) { host.enter(p); }
void probe_host_leave(void *p) { host.leave(p); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return host.create_sem(); }
int xSemaphoreTake(void *p,uint32_t ms) { return host.take(p,ms); }
int xSemaphoreGive(void *p) { return host.give(p); }
void vSemaphoreDelete(void *p) { host.delete_sem(p); }
int xTaskCreate(void (*entry)(void *),const char *name,uint32_t stack,void *arg,int priority,void *out) {
    (void)name;(void)stack;(void)priority;(void)out;return host.spawn((void *)entry,arg);
}
void vTaskDelay(uint32_t ms) { host.sleep_ms(ms); }
void vTaskDelete(void *p) { (void)p; }
unsigned uxTaskGetStackHighWaterMark(void *p) { (void)p; return 0; }
int64_t esp_timer_get_time(void) { return host.now_us(); }
const char *esp_err_to_name(int error) { (void)error; return "host-shim"; }
void probe_host_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
