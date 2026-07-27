/*
 * power_service.c - Power management service implementation
 */

#include "power_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "power_driver.h"
#include "servo_service.h"
#include "led_service.h"
#include "lcd_service.h"
#include "sensor_service.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "power_service";

static bool s_initialized = false;
static bool s_shutdown_in_progress = false;
static power_mode_t s_current_mode = POWER_MODE_NORMAL;
static uint8_t s_normal_brightness_level = LED_BRIGHTNESS_LEVEL_MAX;

/* Battery ADC fallback configuration.
 * Override these macros from project_config.h / system_config.h if the
 * hardware uses a different ADC unit/channel or voltage divider. */
#ifndef POWER_BATTERY_ADC_UNIT
#define POWER_BATTERY_ADC_UNIT ADC_UNIT_2
#endif

#ifndef POWER_BATTERY_ADC_CHANNEL
#define POWER_BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#endif

#ifndef POWER_BATTERY_ADC_BITWIDTH
#define POWER_BATTERY_ADC_BITWIDTH ADC_BITWIDTH_12
#endif

#ifndef POWER_BATTERY_ADC_ATTEN
#define POWER_BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#endif

#ifndef POWER_BATTERY_VOLTAGE_MIN_MV
#define POWER_BATTERY_VOLTAGE_MIN_MV 3300
#endif

#ifndef POWER_BATTERY_VOLTAGE_MAX_MV
#define POWER_BATTERY_VOLTAGE_MAX_MV 4200
#endif

#ifndef POWER_BATTERY_ADC_DIVIDER_NUM
#define POWER_BATTERY_ADC_DIVIDER_NUM 1
#endif

#ifndef POWER_BATTERY_ADC_DIVIDER_DEN
#define POWER_BATTERY_ADC_DIVIDER_DEN 1
#endif

/* ===================== Event Callbacks ===================== */

static void power_service_event_handler(event_t *event, void *context)
{
    (void)context;

    ESP_LOGI(TAG, "[DBG][PWR] event=%d mode=%d", event->type, s_current_mode);
    switch (event->type) {
        case EV_POWER_ON:
            ESP_LOGI(TAG, "Power ON event received");
            power_service_set_power_mode(POWER_MODE_NORMAL);
            break;
        case EV_POWER_OFF:
            ESP_LOGI(TAG, "Power OFF event received");
            power_service_shutdown();
            break;
        case EV_POWER_LOW_BATTERY:
            ESP_LOGW(TAG, "Low battery event received, switching to saving mode");
            power_service_set_power_mode(POWER_MODE_SAVING);
            break;
        case EV_POWER_CHARGING:
            ESP_LOGI(TAG, "Charging event received");
            if (s_current_mode == POWER_MODE_SLEEP || s_current_mode == POWER_MODE_SAVING) {
                ESP_LOGI(TAG, "Leaving %s mode while charging",
                         (s_current_mode == POWER_MODE_SLEEP) ? "sleep" : "saving");
                power_service_set_power_mode(POWER_MODE_NORMAL);
            }
            break;
        case EV_POWER_CHARGED:
            ESP_LOGI(TAG, "Battery fully charged event received");
            break;
        case EV_POWER_OVER_CURRENT:
            ESP_LOGE(TAG, "Over-current event received, shutting down");
            power_service_shutdown();
            break;
        default:
            break;
    }
}

/* ===================== Public API ===================== */

esp_err_t power_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing power service...");

    esp_err_t ret = power_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Power driver init failed");
        return ret;
    }

    /* Subscribe to power-related events */
    ret = event_bus_subscribe(EV_POWER_ON, power_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_POWER_OFF, power_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_POWER_LOW_BATTERY, power_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_POWER_CHARGING, power_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_POWER_CHARGED, power_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_POWER_OVER_CURRENT, power_service_event_handler, NULL);
    (void)ret;

    s_initialized = true;
    ESP_LOGI(TAG, "Power service initialized");
    return ESP_OK;
}

esp_err_t power_service_set_power_mode(power_mode_t mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == s_current_mode) {
        ESP_LOGI(TAG, "Power mode already %d", mode);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Setting power mode: %d -> %d", s_current_mode, mode);
    s_current_mode = mode;

    switch (mode) {
        case POWER_MODE_NORMAL:
            /* Restore full power: LED brightness and sensor monitoring */
            led_service_set_brightness_level(s_normal_brightness_level);
            sensor_service_start_monitoring();
            break;
        case POWER_MODE_SAVING:
            /* Dim LEDs to minimum visible level and reduce sensor sampling */
            s_normal_brightness_level = led_service_get_brightness_level();
            led_service_set_brightness_level(1);
            sensor_service_stop_monitoring();
            break;
        case POWER_MODE_SLEEP:
            /* Notify other services, turn off peripherals, and enter deep sleep */
            ESP_LOGI(TAG, "[DBG][PWR] enter sleep path, publish EV_LCD_SLEEP then call lcd_service_sleep");
            event_bus_publish_simple(EV_AUDIO_STOP);
            event_bus_publish_simple(EV_LCD_SLEEP);

            led_service_turn_off();
            lcd_service_sleep();
            servo_service_disable();
            sensor_service_stop_monitoring();

            ESP_LOGI(TAG, "Entering deep sleep");
            esp_deep_sleep_start();
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

power_mode_t power_service_get_power_mode(void)
{
    return s_current_mode;
}

void power_service_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    if (s_shutdown_in_progress) {
        return;
    }
    s_shutdown_in_progress = true;

    ESP_LOGW(TAG, "System shutdown initiated");

    /* Notify all services to prepare for shutdown */
    event_t ev = {
        .type = EV_SYS_SHUTDOWN,
        .data = NULL,
        .data_size = 0,
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);

    /* Disable high-power peripherals */
    servo_service_disable();
    led_service_turn_off();
    ESP_LOGW(TAG, "[DBG][PWR] shutdown forcing lcd brightness=0");
    lcd_service_set_brightness(0);

    /* Cut ESP module power if the driver supports it */
    power_driver_set_esp_power(false);
}

uint8_t power_service_get_battery_level(void)
{
    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = POWER_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    if (adc_oneshot_new_unit(&init_cfg, &adc_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create battery ADC unit");
        return 0xFF;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = POWER_BATTERY_ADC_BITWIDTH,
        .atten    = POWER_BATTERY_ADC_ATTEN,
    };

    if (adc_oneshot_config_channel(adc_handle, POWER_BATTERY_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure battery ADC channel");
        adc_oneshot_del_unit(adc_handle);
        return 0xFF;
    }

    int raw = 0;
    if (adc_oneshot_read(adc_handle, POWER_BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read battery ADC");
        adc_oneshot_del_unit(adc_handle);
        return 0xFF;
    }

    adc_oneshot_del_unit(adc_handle);

    /* ADC full scale is ~3300 mV with ADC_ATTEN_DB_12 on a 12-bit converter.
     * Apply the configured fixed voltage divider to recover battery voltage. */
    uint32_t measured_mv = ((uint32_t)raw * 3300U) / 4095U;
    uint32_t battery_mv = (measured_mv * POWER_BATTERY_ADC_DIVIDER_DEN) /
                          POWER_BATTERY_ADC_DIVIDER_NUM;

    if (battery_mv <= POWER_BATTERY_VOLTAGE_MIN_MV) {
        return 0;
    }
    if (battery_mv >= POWER_BATTERY_VOLTAGE_MAX_MV) {
        return 100;
    }

    return (uint8_t)(((battery_mv - POWER_BATTERY_VOLTAGE_MIN_MV) * 100U) /
                     (POWER_BATTERY_VOLTAGE_MAX_MV - POWER_BATTERY_VOLTAGE_MIN_MV));
}
