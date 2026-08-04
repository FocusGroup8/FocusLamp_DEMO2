#ifndef SERVO_CONTROL_MODULE_H
#define SERVO_CONTROL_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "servo_control_module_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (SERVO_CONTROL_MODULE_ENABLE == 1)

    typedef enum
    {
        SERVO_CONTROL_STATE_IDLE,
        SERVO_CONTROL_STATE_RECORDING,
        SERVO_CONTROL_STATE_HAS_DATA,
        SERVO_CONTROL_STATE_PLAYING
    } servo_control_state_t;

    typedef struct
    {
        int16_t  lx_pos[4];
        int16_t  em3_pos;
        uint32_t time_ms;
    } servo_control_frame_t;

    typedef struct
    {
        servo_control_state_t current_state;
        int                   current_slot;
        int                   frame_count;
    } servo_control_status_t;

    esp_err_t servo_control_init(void);
    esp_err_t servo_control_deinit(void);

    servo_control_status_t servo_control_get_status(void);
    int                    servo_control_get_frame_count(int slot);

    void servo_control_start_recording(void);
    void servo_control_stop_recording(void);

    void servo_control_start_playback(void);
    void servo_control_stop_playback(void);

    void servo_control_switch_slot(int slot);

    void servo_control_task(void* arg);

    void servo_control_enable_log(void);
    void servo_control_disable_log(void);
    bool servo_control_is_log_enabled(void);

    typedef struct
    {
        uint16_t sampling_ms;
        uint16_t playback_speed;
        uint16_t max_frames;
        uint8_t  max_slots;
    } servo_control_params_t;

    esp_err_t servo_control_set_params(servo_control_params_t* params);
    esp_err_t servo_control_get_params(servo_control_params_t* params);

#else

typedef enum
{
    SERVO_CONTROL_STATE_IDLE,
    SERVO_CONTROL_STATE_RECORDING,
    SERVO_CONTROL_STATE_HAS_DATA,
    SERVO_CONTROL_STATE_PLAYING
} servo_control_state_t;

typedef struct
{
    int16_t  lx_pos[4];
    int16_t  em3_pos;
    uint32_t time_ms;
} servo_control_frame_t;

typedef struct
{
    servo_control_state_t current_state;
    int                   current_slot;
    int                   frame_count;
} servo_control_status_t;

static inline esp_err_t servo_control_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t servo_control_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline servo_control_status_t servo_control_get_status(void)
{
    servo_control_status_t status = {0};
    return status;
}

static inline int servo_control_get_frame_count(int slot)
{
    (void)slot;
    return 0;
}

static inline void servo_control_start_recording(void)
{
}

static inline void servo_control_stop_recording(void)
{
}

static inline void servo_control_start_playback(void)
{
}

static inline void servo_control_stop_playback(void)
{
}

static inline void servo_control_switch_slot(int slot)
{
    (void)slot;
}

static inline void servo_control_task(void* arg)
{
    (void)arg;
}

static inline void servo_control_enable_log(void)
{
}

static inline void servo_control_disable_log(void)
{
}

static inline bool servo_control_is_log_enabled(void)
{
    return false;
}

typedef struct
{
    uint16_t sampling_ms;
    uint16_t playback_speed;
    uint16_t max_frames;
    uint8_t  max_slots;
} servo_control_params_t;

static inline esp_err_t servo_control_set_params(servo_control_params_t* params)
{
    (void)params;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t servo_control_get_params(servo_control_params_t* params)
{
    (void)params;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
