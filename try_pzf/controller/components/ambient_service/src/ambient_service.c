/* Local ambient-light producer. ADC work never runs in the LED/HTTP tasks.
 * Reuses base's average + EMA idea, not its radar/task/event scaffolding. */
#include "ambient_service.h"

#include <math.h>
#include "device_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "light_sensor_driver.h"

#define AMBIENT_BATCH_SAMPLES 8
#define AMBIENT_EMA_ALPHA 0.5f
#define AMBIENT_TASK_STACK 3072
#define AMBIENT_TASK_PRIORITY 1

static const char *TAG = "ambient_service";
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static ambient_snapshot_t s_cache;
/* Startup-task owned; the sampler and readers do not use this flag. */
static bool s_started;

/* Keep lux-to-environment-level thresholds in one controller location.
 * These are estimated lux, not calibrated photometric measurements. */
static uint8_t lux_to_level(float lux)
{
    if (lux > 1000.0f) return 4;
    if (lux > 500.0f) return 3;
    if (lux > 100.0f) return 2;
    if (lux > 20.0f) return 1;
    return 0;
}

esp_err_t ambient_service_get_snapshot(ambient_snapshot_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cache_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_cache_lock);
    int64_t now_ms = esp_timer_get_time() / 1000;
    out->age_ms = out->valid && now_ms >= out->last_ok_ms ? now_ms - out->last_ok_ms : -1;
    out->fresh = out->valid && out->age_ms >= 0 && out->age_ms <= AMBIENT_STALE_TIMEOUT_MS;
    return ESP_OK;
}

static void publish_sample(float filtered_lux, float block_average, int64_t sample_ms)
{
    uint8_t level = lux_to_level(filtered_lux);
    portENTER_CRITICAL(&s_cache_lock);
    s_cache.valid = true;
    s_cache.lux = filtered_lux;
    s_cache.level = level;
    s_cache.last_ok_ms = sample_ms;
    portEXIT_CRITICAL(&s_cache_lock);

    /* Mirror only actual new filtered samples. HTTP/LED readers never write
     * the ambient state or move this sample's timestamp forward. */
    esp_err_t ret = device_state_set_ambient_light(level, filtered_lux);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Ambient state mirror failed: %s", esp_err_to_name(ret));
    ESP_LOGI(TAG, "Ambient light: %.2f lux (avg %.2f, level %u) [EMA sample_age_ms=%lld]",
             (double)filtered_lux, (double)block_average, (unsigned)level,
             (long long)(esp_timer_get_time() / 1000 - sample_ms));
}

static void ambient_task(void *arg)
{
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(AMBIENT_SAMPLE_INTERVAL_MS);
    if (period == 0) period = 1;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t block_start_ms = esp_timer_get_time() / 1000;
    int64_t last_batch_ms = 0;
    int64_t last_failure_log_ms = -5000;
    float block_sum = 0.0f;
    uint32_t block_count = 0;

    for (;;) {
        float lux = 0.0f;
        esp_err_t ret = light_sensor_driver_read_averaged(&lux, AMBIENT_BATCH_SAMPLES);
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool sample_ok = ret == ESP_OK && isfinite(lux) && lux >= 0.0f;
        ambient_snapshot_t previous;
        (void)ambient_service_get_snapshot(&previous);

        if (sample_ok && !previous.fresh) {
            /* First good batch, or recovery after stale data: do not blend
             * a new environment with an expired historical EMA. */
            publish_sample(lux, lux, now_ms);
            block_sum = 0.0f;
            block_count = 0;
            block_start_ms = now_ms;
        } else {
            if (sample_ok) {
                block_sum += lux;
                ++block_count;
                last_batch_ms = now_ms;
            }
            if (now_ms - block_start_ms >= AMBIENT_BLOCK_INTERVAL_MS) {
                /* Average successful batches in this real-time window.
                 * Failure is not a zero-lux sample. An empty window does not
                 * republish history or refresh its validity timestamp. */
                if (block_count > 0) {
                    float average = block_sum / (float)block_count;
                    float filtered = previous.fresh
                        ? AMBIENT_EMA_ALPHA * average + (1.0f - AMBIENT_EMA_ALPHA) * previous.lux
                        : average;
                    publish_sample(filtered, average, last_batch_ms);
                }
                block_sum = 0.0f;
                block_count = 0;
                block_start_ms = now_ms;
            }
        }

        if (!sample_ok && now_ms - last_failure_log_ms >= 5000) {
            ESP_LOGW(TAG, "ADC batch unavailable: %s; cached sample expires after %dms",
                     ret == ESP_OK ? "invalid lux" : esp_err_to_name(ret), AMBIENT_STALE_TIMEOUT_MS);
            last_failure_log_ms = now_ms;
        }

        /* No burst catch-up if an ADC read or scheduling gap stretched a
         * period. Block timing and sample age still use actual monotonic time. */
        TickType_t now_ticks = xTaskGetTickCount();
        if ((now_ticks - last_wake) >= period) last_wake = now_ticks;
        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t ambient_service_start(void)
{
    if (s_started) return ESP_OK;
    esp_err_t ret = light_sensor_driver_init();
    if (ret != ESP_OK) return ret;
    if (xTaskCreate(ambient_task, "ambient_service", AMBIENT_TASK_STACK, NULL,
                    AMBIENT_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "Ambient service started: sample=%dms block=%dms EMA=%.2f expiry=%dms priority=%d",
             AMBIENT_SAMPLE_INTERVAL_MS, AMBIENT_BLOCK_INTERVAL_MS, (double)AMBIENT_EMA_ALPHA,
             AMBIENT_STALE_TIMEOUT_MS, AMBIENT_TASK_PRIORITY);
    return ESP_OK;
}
