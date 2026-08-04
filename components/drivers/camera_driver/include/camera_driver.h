#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "camera_driver_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint32_t width;
        uint32_t height;
        uint32_t buffer_num;
        uint32_t pixel_format;
    } camera_config_t;

    typedef struct
    {
        uint8_t* data;
        size_t   size;
        uint32_t index;
    } camera_frame_t;

#if (CAMERA_DRIVER_ENABLE == 1)

    esp_err_t camera_driver_init(const camera_config_t* config);
    esp_err_t camera_driver_start(void);
    esp_err_t camera_driver_stop(void);
    esp_err_t camera_driver_capture_frame(camera_frame_t* frame);
    esp_err_t camera_driver_release_frame(camera_frame_t* frame);
    void      camera_driver_get_resolution(uint32_t* width, uint32_t* height);
    bool      camera_driver_is_ready(void);
    esp_err_t camera_driver_deinit(void);

#else

static inline esp_err_t camera_driver_init(const camera_config_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t camera_driver_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t camera_driver_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t camera_driver_capture_frame(camera_frame_t* frame)
{
    (void)frame;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t camera_driver_release_frame(camera_frame_t* frame)
{
    (void)frame;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void camera_driver_get_resolution(uint32_t* width, uint32_t* height)
{
    if (width)
        *width = 0;
    if (height)
        *height = 0;
}

static inline bool camera_driver_is_ready(void)
{
    return false;
}

static inline esp_err_t camera_driver_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
