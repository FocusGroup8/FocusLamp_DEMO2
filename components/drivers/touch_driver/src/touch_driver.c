/*
 * touch_driver.c - Capacitive touch sensor driver implementation
 */

#include "touch_driver.h"
#include "pin_config.h"
#include "bsp_gpio.h"

#include <string.h>
#include "driver/gpio.h"

/* Touch GPIO pin mapping */
static const gpio_num_t s_touch_gpios[TOUCH_POINT_MAX] = {
    TTP_A_GPIO,  /* TOUCH_POINT_A */
    TTP_B_GPIO,  /* TOUCH_POINT_B */
    TTP_C_GPIO,  /* TOUCH_POINT_C */
    TTP_D_GPIO,  /* TOUCH_POINT_D */
};

/* Internal touch state buffer */
static bool s_touch_states[TOUCH_POINT_MAX] = { false };
static bool s_raw_states[TOUCH_POINT_MAX]   = { false };
static uint8_t s_debounce_counters[TOUCH_POINT_MAX] = { 0 };

esp_err_t touch_driver_init(void)
{
    /* Configure touch GPIOs as input (TTP223 outputs high on touch)
     * Enable internal pull-down so unconnected pins (no TTP223 installed)
     * stay LOW instead of floating and causing false touch triggers. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TTP_A_GPIO) |
                        (1ULL << TTP_B_GPIO) |
                        (1ULL << TTP_C_GPIO) |
                        (1ULL << TTP_D_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Initialize internal state */
    memset(s_touch_states, 0, sizeof(s_touch_states));
    memset(s_raw_states, 0, sizeof(s_raw_states));
    memset(s_debounce_counters, 0, sizeof(s_debounce_counters));

    return ESP_OK;
}

void touch_driver_scan(void)
{
    for (int i = 0; i < TOUCH_POINT_MAX; i++) {
        int level = bsp_gpio_get_level(s_touch_gpios[i]);
        bool raw = (level == 1);

        if (raw == s_raw_states[i]) {
            /* Same state: increment debounce counter */
            if (s_debounce_counters[i] < TOUCH_DRIVER_DEBOUNCE_COUNT) {
                s_debounce_counters[i]++;
            }
            /* Update stable state once debounce count reached */
            if (s_debounce_counters[i] >= TOUCH_DRIVER_DEBOUNCE_COUNT) {
                s_touch_states[i] = raw;
            }
        } else {
            /* State changed: reset counter and remember new raw state */
            s_raw_states[i] = raw;
            s_debounce_counters[i] = 0;
        }
    }
}

bool touch_driver_get_state(touch_point_t point)
{
    if (point >= TOUCH_POINT_MAX) {
        return false;
    }
    return s_touch_states[point];
}

uint8_t touch_driver_get_active_points(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < TOUCH_POINT_MAX; i++) {
        if (s_touch_states[i]) {
            mask |= (1 << i);
        }
    }
    return mask;
}