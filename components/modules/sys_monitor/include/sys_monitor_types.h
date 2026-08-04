#ifndef SYS_MONITOR_TYPES_H
#define SYS_MONITOR_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint32_t total_free;
        uint32_t internal_free;
        uint32_t internal_largest_free_block;
        uint32_t internal_min_ever_free;
        uint32_t psram_free;
        uint32_t psram_largest_free_block;
        uint32_t psram_min_ever_free;
    } sys_monitor_heap_info_t;

    typedef struct
    {
        char     name[16];
        uint32_t runtime_percent;
        uint32_t absolute_runtime;
        uint32_t stack_high_water_mark;
        uint32_t current_priority;
        uint32_t task_number;
    } sys_monitor_task_info_t;

    typedef struct
    {
        sys_monitor_heap_info_t heap;
        uint32_t                task_count;
        uint32_t                uptime_ms;
    } sys_monitor_stats_t;

#ifdef __cplusplus
}
#endif

#endif
