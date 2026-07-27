/*
 * bsp_i2s.c - I2S initialization and communication for FocusLamp BSP
 *
 * 注意：I2S 硬件现已由 audio_bridge 组件统一接管（TX+RX 全双工，
 * 16kHz/MONO/32bit，用于 OPUS 语音解码与麦克风编码）。
 * bsp_i2s_init() 保留为 no-op 以维持调用链兼容；bsp_i2s_write/read
 * 不再直接操作硬件，调用方应改用 audio_bridge 接口。
 */

#include "bsp_i2s.h"
#include "pin_config.h"
#include "system_config.h"
#include "esp_log.h"

/* I2S port */
#define BSP_I2S_PORT    I2S_NUM_0

static const char *TAG = "bsp_i2s";

esp_err_t bsp_i2s_init(void)
{
    /* audio_bridge 在 audio_driver_init 阶段已创建 I2S 通道，
     * 此处不再重复初始化，避免 I2S_NUM_0 双重占用。 */
    ESP_LOGW(TAG, "bsp_i2s_init: skipped (I2S owned by audio_bridge)");
    return ESP_OK;
}

esp_err_t bsp_i2s_write(const uint8_t *data, size_t len)
{
    /* I2S 已由 audio_bridge 接管，此接口仅保留兼容性。
     * 调用方（如 audio_driver）应直接使用 audio_bridge_write_pcm。 */
    (void)data;
    (void)len;
    ESP_LOGW(TAG, "bsp_i2s_write: deprecated, use audio_bridge_write_pcm");
    return ESP_ERR_NOT_SUPPORTED;
}

int bsp_i2s_read(uint8_t *data, size_t len, TickType_t timeout)
{
    /* I2S 已由 audio_bridge 接管，此接口仅保留兼容性。
     * 调用方（如 audio_driver）应直接使用 audio_bridge_read_pcm。 */
    (void)data;
    (void)len;
    (void)timeout;
    return -1;
}
