#ifndef TOUCH_INPUT_DRIVER_H
#define TOUCH_INPUT_DRIVER_H

#include <stdbool.h>

#include "esp_err.h"

#include "touch_input_driver_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (TOUCH_INPUT_DRIVER_ENABLE == 1)

    typedef enum
    {
        TOUCH_INPUT_A   = 0,
        TOUCH_INPUT_B   = 1,
        TOUCH_INPUT_C   = 2,
        TOUCH_INPUT_MAX = 3
    } touch_input_t;

    typedef struct
    {
        bool pressed[TOUCH_INPUT_MAX];
    } touch_input_state_t;

    esp_err_t touch_input_init(void);
    esp_err_t touch_input_deinit(void);

    touch_input_state_t touch_input_read(void);
    bool                touch_input_is_pressed(touch_input_t input);

#else

typedef enum
{
    TOUCH_INPUT_A   = 0,
    TOUCH_INPUT_B   = 1,
    TOUCH_INPUT_C   = 2,
    TOUCH_INPUT_MAX = 3
} touch_input_t;

typedef struct
{
    bool pressed[TOUCH_INPUT_MAX];
} touch_input_state_t;

static inline esp_err_t touch_input_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t touch_input_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline touch_input_state_t touch_input_read(void)
{
    touch_input_state_t state = {0};
    return state;
}

static inline bool touch_input_is_pressed(touch_input_t input)
{
    (void)input;
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif