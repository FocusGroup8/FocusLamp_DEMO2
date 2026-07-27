#ifndef CAMERA_DRIVER_CONFIG_H
#define CAMERA_DRIVER_CONFIG_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CAMERA_DRIVER_ENABLE CONFIG_PROJECT_ENABLE_CAMERA

#ifndef CAMERA_DRIVER_ENABLE
#define CAMERA_DRIVER_ENABLE 0
#endif

#if (CAMERA_DRIVER_ENABLE == 1)

#include "camera_config.h"

#ifndef CAMERA_HRES_DEFAULT
#define CAMERA_HRES_DEFAULT 800
#endif

#ifndef CAMERA_VRES_DEFAULT
#define CAMERA_VRES_DEFAULT 640
#endif

#ifndef CAMERA_BUFFER_NUM
#define CAMERA_BUFFER_NUM 3
#endif

#ifndef MIPI_CSI_CAM_SCCB_SCL_IO
#define MIPI_CSI_CAM_SCCB_SCL_IO 8
#endif

#ifndef MIPI_CSI_CAM_SCCB_SDA_IO
#define MIPI_CSI_CAM_SCCB_SDA_IO 7
#endif

#ifndef LDO_CHAN_ID
#define LDO_CHAN_ID 3
#endif

#ifndef LDO_VOLTAGE_MV
#define LDO_VOLTAGE_MV 2500
#endif

#include "esp_video_device.h"
#define VIDEO_DEVICE_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME

#endif

#ifdef __cplusplus
}
#endif

#endif
