/*
 * bsp_adc.c - ADC initialization and reading for FocusLamp BSP
 */

#include "bsp_adc.h"
#include "pin_config.h"
#include "esp_adc/adc_oneshot.h"

/* ADC handle and channel config */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_channel_t s_adc_channel;

/* ADC configuration constants */
#define BSP_ADC_UNIT        ADC_UNIT_1
#define BSP_ADC_BITWIDTH    ADC_BITWIDTH_12
#define BSP_ADC_ATTEN       ADC_ATTEN_DB_12   /* 0-3.3V input range */

esp_err_t bsp_adc_init(void)
{
    esp_err_t ret;

    if (s_adc_handle != NULL) {
        return ESP_OK;  /* Already initialized */
    }

    /* Oneshot ADC unit configuration */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BSP_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ret = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (ret != ESP_OK) return ret;

    /* Map TEMT_OUT_GPIO (GPIO21) to ADC1 channel 5 on ESP32-P4 */
    ret = adc_oneshot_config_channel(s_adc_handle,
                                     ADC_CHANNEL_5,  /* ADC1_CH5 = GPIO21 = TEMT_OUT_GPIO */
                                     &(adc_oneshot_chan_cfg_t){
                                         .bitwidth = BSP_ADC_BITWIDTH,
                                         .atten    = BSP_ADC_ATTEN,
                                     });
    if (ret != ESP_OK) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    /* Store the channel for later use */
    s_adc_channel = ADC_CHANNEL_5;

    return ESP_OK;
}

adc_oneshot_unit_handle_t bsp_adc_get_handle(void)
{
    return s_adc_handle;
}

adc_channel_t bsp_adc_get_light_channel(void)
{
    return s_adc_channel;
}

esp_err_t bsp_adc_read_light(uint32_t *value)
{
    if (s_adc_handle == NULL || value == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = (uint32_t)raw;
    return ESP_OK;
}