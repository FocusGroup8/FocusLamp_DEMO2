#ifndef JPEG_ENCODER_H
#define JPEG_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "jpeg_encoder_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (JPEG_ENCODER_ENABLE == 1)

    esp_err_t jpeg_encoder_init(uint32_t width, uint32_t height, uint32_t quality);
    esp_err_t jpeg_encode_rgb565(uint8_t* input, uint8_t* output, size_t* output_size);
    esp_err_t jpeg_encoder_deinit(void);
    bool      jpeg_encoder_is_ready(void);
    uint32_t  jpeg_encoder_get_width(void);
    uint32_t  jpeg_encoder_get_height(void);

#else

static inline esp_err_t jpeg_encoder_init(uint32_t width, uint32_t height, uint32_t quality)
{
    (void)width;
    (void)height;
    (void)quality;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t jpeg_encode_rgb565(uint8_t* input, uint8_t* output, size_t* output_size)
{
    (void)input;
    (void)output;
    (void)output_size;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t jpeg_encoder_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline bool jpeg_encoder_is_ready(void)
{
    return false;
}

static inline uint32_t jpeg_encoder_get_width(void)
{
    return 0;
}

static inline uint32_t jpeg_encoder_get_height(void)
{
    return 0;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
