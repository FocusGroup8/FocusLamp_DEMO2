#ifndef JPEG_ENCODER_CONFIG_H
#define JPEG_ENCODER_CONFIG_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define JPEG_ENCODER_ENABLE CONFIG_PROJECT_ENABLE_CAMERA

#ifndef JPEG_ENCODER_ENABLE
#define JPEG_ENCODER_ENABLE 0
#endif

#if (JPEG_ENCODER_ENABLE == 1)

#include "camera_config.h"

#ifndef JPEG_OUTPUT_BUFFER_SIZE
#define JPEG_OUTPUT_BUFFER_SIZE (150 * 1024)
#endif

#ifndef JPEG_QUALITY_DEFAULT
#define JPEG_QUALITY_DEFAULT 10
#endif

#include "esp_video_device.h"
#define JPEG_DEVICE_PATH ESP_VIDEO_JPEG_DEVICE_NAME

#endif

#ifdef __cplusplus
}
#endif

#endif
