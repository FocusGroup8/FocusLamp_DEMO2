/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "touch_sensor_config.h"
#include "touch_sensor_types.h"

#if (TOUCH_SENSOR_ENABLE == 1)

static const char *TAG = "TOUCH_SENSOR";

/*---------------------------------------------------------------
 * State
 *-------------------------------------------------------------*/
static bool s_initialized                 = false;
static touch_sensor_event_cb_t s_event_cb = NULL;
static void *s_user_ctx                   = NULL;
static int64_t s_last_event_time_ms       = 0; /* For software debounce + interval filter */
static touch_event_t s_last_event         = TOUCH_EVENT_RELEASE; /* Last accepted edge direction */

/*---------------------------------------------------------------
 * GPIO ISR handler
 *
 * TTP223 sync mode:
 *   Rising edge (GPIO HIGH) = finger detected → TOUCH_EVENT_PRESS
 *   Falling edge (GPIO LOW) = finger released → TOUCH_EVENT_RELEASE
 *
 * Debounce policy:
 *   - Same-direction edges (PRESS→PRESS / RELEASE→RELEASE) are contact
 *     bounce / noise → filtered by TOUCH_SENSOR_DEBOUNCE_MS.
 *   - Direction changes (PRESS↔RELEASE) are always accepted, otherwise a
 *     quick tap (< TOUCH_SENSOR_MIN_INTERVAL_MS) would have its RELEASE
 *     edge discarded and be misread as a LONG_PRESS.
 *
 * ISR-safe: only captures timestamp and calls callback.
 *-------------------------------------------------------------*/
#define TOUCH_SENSOR_EDGE_GUARD_MS 2 /* Tiny guard vs. double-ISR storm */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;

    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;

    /* Determine event type from current GPIO level */
    int level           = gpio_get_level(TOUCH_SENSOR_GPIO);
    touch_event_t event = (level == 1) ? TOUCH_EVENT_PRESS : TOUCH_EVENT_RELEASE;

    int64_t elapsed = now_ms - s_last_event_time_ms;
    if (event == s_last_event) {
        /* Same direction: debounce against bounce/noise */
        if (elapsed < TOUCH_SENSOR_DEBOUNCE_MS) {
            return;
        }
    } else {
        /* Direction change (press<->release): accept immediately */
        if (elapsed < TOUCH_SENSOR_EDGE_GUARD_MS) {
            return;
        }
    }
    s_last_event_time_ms = now_ms;
    s_last_event         = event;

    /* Deliver event via callback */
    if (s_event_cb) {
        touch_sensor_event_data_t data = {
            .event        = event,
            .timestamp_ms = now_ms,
        };
        s_event_cb(&data, s_user_ctx);
    }
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/

esp_err_t touch_sensor_init(const touch_sensor_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Apply config */
    if (config) {
        s_event_cb = config->event_cb;
        s_user_ctx = config->user_ctx;
    }

    ESP_LOGI(TAG, "Initializing TTP223 touch sensor (GPIO=%d, debounce=%dms, interval=%dms)...", TOUCH_SENSOR_GPIO,
             TOUCH_SENSOR_DEBOUNCE_MS, TOUCH_SENSOR_MIN_INTERVAL_MS);

    /* Configure GPIO input */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TOUCH_SENSOR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install GPIO ISR service (safe to call multiple times) */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add ISR handler for touch sensor GPIO */
    ret = gpio_isr_handler_add(TOUCH_SENSOR_GPIO, gpio_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ISR handler add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized        = true;
    s_last_event_time_ms = 0;
    s_last_event         = TOUCH_EVENT_RELEASE;

    ESP_LOGI(TAG, "TTP223 touch sensor initialized");
    return ESP_OK;
}

esp_err_t touch_sensor_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* Remove ISR handler */
    gpio_isr_handler_remove(TOUCH_SENSOR_GPIO);

    /* Reset GPIO to default */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TOUCH_SENSOR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_initialized        = false;
    s_event_cb           = NULL;
    s_user_ctx           = NULL;
    s_last_event_time_ms = 0;
    s_last_event         = TOUCH_EVENT_RELEASE;

    ESP_LOGI(TAG, "TTP223 touch sensor deinitialized");
    return ESP_OK;
}

bool touch_sensor_is_initialized(void)
{
    return s_initialized;
}

bool touch_sensor_is_pressed(void)
{
    if (!s_initialized) {
        return false;
    }
    return gpio_get_level(TOUCH_SENSOR_GPIO) == 1;
}

#endif /* TOUCH_SENSOR_ENABLE == 1 */
