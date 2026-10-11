/**
 * @file light_sensor_driver.c
 * @brief Light sensor driver implementation
 *
 * This file implements the TEMT6000 light sensor driver using ADC.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#include "light_sensor_driver_config.h"

#include "light_sensor_driver.h"

#if (LIGHT_SENSOR_ENABLE == 1)

#include <math.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char* TAG = "light_sensor";

static bool s_initialized = false;
static adc_oneshot_unit_handle_t s_adc_handle;
static const adc_channel_t s_channel = ADC_CHANNEL_5;

esp_err_t light_sensor_driver_init(void)
{
    if (s_initialized) return ESP_OK;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id=ADC_UNIT_1, .ulp_mode=ADC_ULP_MODE_DISABLE};
    esp_err_t ret = adc_oneshot_new_unit(&unit, &s_adc_handle);
    if (ret != ESP_OK) return ret;
    adc_oneshot_chan_cfg_t channel = {.bitwidth=ADC_BITWIDTH_12, .atten=ADC_ATTEN_DB_12};
    ret = adc_oneshot_config_channel(s_adc_handle, s_channel, &channel);
    if (ret != ESP_OK) {
        adc_oneshot_del_unit(s_adc_handle); s_adc_handle=NULL; return ret;
    }
    s_initialized=true;
    ESP_LOGI(TAG, "ADC1 CH5 GPIO21 ready (estimated lux, not calibrated)");
    return ESP_OK;
}

esp_err_t light_sensor_driver_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    esp_err_t ret=adc_oneshot_del_unit(s_adc_handle);
    if (ret == ESP_OK) { s_adc_handle=NULL; s_initialized=false; }
    return ret;
}

esp_err_t light_sensor_driver_read(float* lux)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (lux == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameter: lux is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_unit_handle_t handle = s_adc_handle;
    adc_channel_t channel = s_channel;

    int       adc_raw = 0;
    esp_err_t ret     = adc_oneshot_read(handle, channel, &adc_raw);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    /* TEMT6000: Vout ∝ lux.  ADC 12-bit → mV → lux.
     * At 3300 mV (full-scale) = 2000 lux max for typical indoor range. */
    float voltage = (float)adc_raw * 3300.0f / 4095.0f;
    *lux          = (voltage / 3300.0f) * 2000.0f;

    ESP_LOGD(TAG, "ADC raw: %d, Voltage: %.2f mV, Lux: %.2f", adc_raw, voltage, *lux);

    return ESP_OK;
}

esp_err_t light_sensor_driver_read_averaged(float* lux, uint8_t sample_count)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (lux == NULL || sample_count == 0)
    {
        ESP_LOGE(TAG, "Invalid parameter");
        return ESP_ERR_INVALID_ARG;
    }

    float sum = 0.0f;
    int valid_samples = 0;

    for (uint8_t i = 0; i < sample_count; i++)
    {
        float sample = 0.0f;
        esp_err_t ret = light_sensor_driver_read(&sample);
        if (ret == ESP_OK)
        {
            sum += sample;
            valid_samples++;
        }
        else
        {
            return ret; /* An eight-sample request must not hide an ADC failure. */
        }
    }

    if (valid_samples == 0)
    {
        return ESP_FAIL;
    }

    *lux = sum / (float)valid_samples;

    ESP_LOGD(TAG, "Averaged lux: %.2f (samples=%d/%d)", *lux, valid_samples, sample_count);

    return ESP_OK;
}

esp_err_t light_sensor_driver_is_dark(bool* is_dark)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (is_dark == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameter: is_dark is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    float     lux = 0.0f;
    esp_err_t ret = light_sensor_driver_read(&lux);
    if (ret != ESP_OK)
    {
        return ret;
    }

    *is_dark = (lux < LIGHT_SENSOR_LUX_THRESHOLD_LOW);
    return ESP_OK;
}

esp_err_t light_sensor_driver_is_bright(bool* is_bright)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (is_bright == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameter: is_bright is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    float     lux = 0.0f;
    esp_err_t ret = light_sensor_driver_read(&lux);
    if (ret != ESP_OK)
    {
        return ret;
    }

    *is_bright = (lux > LIGHT_SENSOR_LUX_THRESHOLD_HIGH);
    return ESP_OK;
}

bool light_sensor_driver_is_initialized(void)
{
    return s_initialized;
}

#endif
