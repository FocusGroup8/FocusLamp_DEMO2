#include "camera_driver_config.h"

#if (CAMERA_DRIVER_ENABLE == 1)

#include <fcntl.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_video_init.h"

#include "camera_driver.h"
#include "driver/i2c_master.h"
#include "linux/videodev2.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

static const char* TAG = "camera_driver";

static int                      s_video_fd = -1;
static uint8_t*                 s_frame_buffers[CAMERA_BUFFER_NUM];
static size_t                   s_frame_buffer_size = 0;
static uint32_t                 s_camera_hres       = 0;
static uint32_t                 s_camera_vres       = 0;
static esp_ldo_channel_handle_t s_ldo_mipi_phy      = NULL;
static bool                     s_is_streaming      = false;

static esp_err_t init_mipi_ldo(void)
{
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id    = LDO_CHAN_ID,
        .voltage_mv = LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_ldo_mipi_phy);
}

static void validate_gpio_config(void)
{
    ESP_LOGI(TAG, "Validating GPIO configuration:");
    ESP_LOGI(TAG, "  SCCB SCL: GPIO %d", MIPI_CSI_CAM_SCCB_SCL_IO);
    ESP_LOGI(TAG, "  SCCB SDA: GPIO %d", MIPI_CSI_CAM_SCCB_SDA_IO);
    ESP_LOGI(TAG, "  LDO Channel: %d", LDO_CHAN_ID);
    ESP_LOGI(TAG, "  LDO Voltage: %d mV", LDO_VOLTAGE_MV);

    if (MIPI_CSI_CAM_SCCB_SCL_IO < 0 || MIPI_CSI_CAM_SCCB_SCL_IO > 63)
    {
        ESP_LOGW(TAG, "Invalid SCCB SCL GPIO: %d (should be 0-63)", MIPI_CSI_CAM_SCCB_SCL_IO);
    }

    if (MIPI_CSI_CAM_SCCB_SDA_IO < 0 || MIPI_CSI_CAM_SCCB_SDA_IO > 63)
    {
        ESP_LOGW(TAG, "Invalid SCCB SDA GPIO: %d (should be 0-63)", MIPI_CSI_CAM_SCCB_SDA_IO);
    }

    ESP_LOGI(TAG, "GPIO configuration validated successfully");
}

static esp_err_t init_video_device(void)
{
    esp_err_t ret;

    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config =
                {
                    .init_sccb = true,
                    .i2c_config =
                        {
                            .port    = I2C_NUM_0,
                            .scl_pin = MIPI_CSI_CAM_SCCB_SCL_IO,
                            .sda_pin = MIPI_CSI_CAM_SCCB_SDA_IO,
                        },
                    .freq = 100000,
                },
            .reset_pin = -1,
            .pwdn_pin  = -1,
        },
    };

    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };

    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize video device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Video device initialized");
    return ESP_OK;
}

static int open_video_device(const char* dev_path, uint32_t pixelformat, uint32_t width,
                             uint32_t height)
{
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "Failed to open video device: %s", dev_path);
        return -1;
    }

    struct v4l2_capability capability;
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0)
    {
        ESP_LOGE(TAG, "Failed to query capability");
        close(fd);
        return -1;
    }

    ESP_LOGI(TAG, "Video device: %s", capability.card);

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width       = width;
    format.fmt.pix.height      = height;
    format.fmt.pix.pixelformat = pixelformat;
    format.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0)
    {
        ESP_LOGW(TAG, "Failed to set format, trying to get current format");

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (ioctl(fd, VIDIOC_G_FMT, &format) != 0)
        {
            ESP_LOGE(TAG, "Failed to get format");
            close(fd);
            return -1;
        }
    }

    s_camera_hres = format.fmt.pix.width;
    s_camera_vres = format.fmt.pix.height;
    ESP_LOGI(TAG, "Camera resolution: %dx%d", s_camera_hres, s_camera_vres);

    s_frame_buffer_size = s_camera_hres * s_camera_vres * 2;
    ESP_LOGI(TAG, "Frame buffer size: %d bytes", s_frame_buffer_size);

    return fd;
}

static esp_err_t setup_video_buffers(int fd, uint32_t buf_num)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = buf_num;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0)
    {
        ESP_LOGE(TAG, "Failed to request buffers");
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < buf_num; i++)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0)
        {
            ESP_LOGE(TAG, "Failed to query buffer %d", i);
            return ESP_FAIL;
        }

        s_frame_buffers[i] =
            mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (s_frame_buffers[i] == MAP_FAILED)
        {
            ESP_LOGE(TAG, "Failed to mmap buffer %d", i);
            return ESP_FAIL;
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0)
        {
            ESP_LOGE(TAG, "Failed to queue buffer %d", i);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Video buffers setup complete: %d buffers", buf_num);
    return ESP_OK;
}

esp_err_t camera_driver_init(const camera_config_t* config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing camera driver");
    ESP_LOGI(TAG, "Target resolution: %dx%d", config->width, config->height);

    validate_gpio_config();

    ret = init_mipi_ldo();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "MIPI LDO initialization failed, continuing anyway");
    }
    else
    {
        ESP_LOGI(TAG, "MIPI LDO initialized successfully");
    }

    ret = init_video_device();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize video device");
        return ret;
    }

    s_video_fd =
        open_video_device(VIDEO_DEVICE_PATH, config->pixel_format, config->width, config->height);
    if (s_video_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to open video device");
        return ESP_FAIL;
    }

    ret = setup_video_buffers(s_video_fd, config->buffer_num);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to setup video buffers");
        close(s_video_fd);
        s_video_fd = -1;
        return ret;
    }

    ESP_LOGI(TAG, "Camera driver initialized successfully");
    return ESP_OK;
}

esp_err_t camera_driver_start(void)
{
    if (s_video_fd < 0)
    {
        ESP_LOGE(TAG, "Camera not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0)
    {
        ESP_LOGE(TAG, "Failed to start stream");
        return ESP_FAIL;
    }

    s_is_streaming = true;
    ESP_LOGI(TAG, "Camera stream started");
    return ESP_OK;
}

esp_err_t camera_driver_stop(void)
{
    if (s_video_fd < 0)
    {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0)
    {
        ESP_LOGE(TAG, "Failed to stop stream");
        return ESP_FAIL;
    }

    s_is_streaming = false;
    ESP_LOGI(TAG, "Camera stream stopped");
    return ESP_OK;
}

esp_err_t camera_driver_capture_frame(camera_frame_t* frame)
{
    if (frame == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_video_fd < 0 || !s_is_streaming)
    {
        ESP_LOGE(TAG, "Camera not ready");
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0)
    {
        ESP_LOGW(TAG, "Failed to dequeue buffer");
        return ESP_FAIL;
    }

    frame->data  = s_frame_buffers[buf.index];
    frame->size  = buf.bytesused;
    frame->index = buf.index;

    return ESP_OK;
}

esp_err_t camera_driver_release_frame(camera_frame_t* frame)
{
    if (frame == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_video_fd < 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = frame->index;

    if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0)
    {
        ESP_LOGW(TAG, "Failed to queue buffer");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void camera_driver_get_resolution(uint32_t* width, uint32_t* height)
{
    if (width)
        *width = s_camera_hres;
    if (height)
        *height = s_camera_vres;
}

bool camera_driver_is_ready(void)
{
    return s_video_fd >= 0 && s_is_streaming;
}

esp_err_t camera_driver_deinit(void)
{
    if (s_video_fd < 0)
    {
        return ESP_OK;
    }

    if (s_is_streaming)
    {
        camera_driver_stop();
    }

    for (int i = 0; i < CAMERA_BUFFER_NUM; i++)
    {
        if (s_frame_buffers[i] != NULL && s_frame_buffers[i] != MAP_FAILED)
        {
            munmap(s_frame_buffers[i], s_frame_buffer_size);
            s_frame_buffers[i] = NULL;
        }
    }

    close(s_video_fd);
    s_video_fd          = -1;
    s_camera_hres       = 0;
    s_camera_vres       = 0;
    s_frame_buffer_size = 0;

    if (s_ldo_mipi_phy != NULL)
    {
        esp_ldo_release_channel(s_ldo_mipi_phy);
        s_ldo_mipi_phy = NULL;
    }

    ESP_LOGI(TAG, "Camera driver deinitialized");
    return ESP_OK;
}

#endif
