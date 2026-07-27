#include "jpeg_encoder_config.h"

#if (JPEG_ENCODER_ENABLE == 1)

#include <fcntl.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_device.h"

#include "jpeg_encoder.h"
#include "linux/videodev2.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

static const char* TAG = "jpeg_encoder";

static int      s_jpeg_fd            = -1;
static uint32_t s_width              = 0;
static uint32_t s_height             = 0;
static uint32_t s_quality            = 70;
static uint8_t* s_output_buffer      = NULL;
static size_t   s_output_buffer_size = 0;

esp_err_t jpeg_encoder_init(uint32_t width, uint32_t height, uint32_t quality)
{
    if (s_jpeg_fd >= 0)
    {
        ESP_LOGW(TAG, "JPEG encoder already initialized");
        return ESP_OK;
    }

    s_width   = width;
    s_height  = height;
    s_quality = quality;

    ESP_LOGI(TAG, "Initializing JPEG encoder: %dx%d, quality=%d", width, height, quality);

    s_jpeg_fd = open(JPEG_DEVICE_PATH, O_RDONLY);
    if (s_jpeg_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to open JPEG device: %s", JPEG_DEVICE_PATH);
        return ESP_FAIL;
    }

    struct v4l2_capability capability;
    if (ioctl(s_jpeg_fd, VIDIOC_QUERYCAP, &capability) != 0)
    {
        ESP_LOGE(TAG, "Failed to query JPEG capability");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JPEG device: %s", capability.card);

    struct v4l2_ext_controls controls;
    struct v4l2_ext_control  control;

    controls.ctrl_class = V4L2_CID_JPEG_CLASS;
    controls.count      = 1;
    controls.controls   = &control;
    control.id          = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    control.value       = quality;

    if (ioctl(s_jpeg_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0)
    {
        ESP_LOGW(TAG, "Failed to set JPEG compression quality");
    }
    else
    {
        ESP_LOGI(TAG, "JPEG quality set to %d", quality);
    }

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width       = width;
    format.fmt.pix.height      = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    format.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(s_jpeg_fd, VIDIOC_S_FMT, &format) != 0)
    {
        ESP_LOGE(TAG, "Failed to set JPEG output format");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(s_jpeg_fd, VIDIOC_REQBUFS, &req) != 0)
    {
        ESP_LOGE(TAG, "Failed to request JPEG output buffers");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    memset(&format, 0, sizeof(format));
    format.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width       = width;
    format.fmt.pix.height      = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    format.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(s_jpeg_fd, VIDIOC_S_FMT, &format) != 0)
    {
        ESP_LOGE(TAG, "Failed to set JPEG capture format");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_jpeg_fd, VIDIOC_REQBUFS, &req) != 0)
    {
        ESP_LOGE(TAG, "Failed to request JPEG capture buffers");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = 0;

    if (ioctl(s_jpeg_fd, VIDIOC_QUERYBUF, &buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to query JPEG capture buffer");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    s_output_buffer =
        mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_jpeg_fd, buf.m.offset);
    if (s_output_buffer == MAP_FAILED)
    {
        ESP_LOGE(TAG, "Failed to mmap JPEG output buffer");
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    s_output_buffer_size = buf.length;

    if (ioctl(s_jpeg_fd, VIDIOC_QBUF, &buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to queue JPEG capture buffer");
        munmap(s_output_buffer, s_output_buffer_size);
        s_output_buffer = NULL;
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_jpeg_fd, VIDIOC_STREAMON, &type) != 0)
    {
        ESP_LOGE(TAG, "Failed to start JPEG capture stream");
        munmap(s_output_buffer, s_output_buffer_size);
        s_output_buffer = NULL;
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(s_jpeg_fd, VIDIOC_STREAMON, &type) != 0)
    {
        ESP_LOGE(TAG, "Failed to start JPEG output stream");
        int cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_jpeg_fd, VIDIOC_STREAMOFF, &cap_type);
        munmap(s_output_buffer, s_output_buffer_size);
        s_output_buffer = NULL;
        close(s_jpeg_fd);
        s_jpeg_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JPEG encoder initialized successfully");
    return ESP_OK;
}

esp_err_t jpeg_encode_rgb565(uint8_t* input, uint8_t* output, size_t* output_size)
{
    if (s_jpeg_fd < 0)
    {
        ESP_LOGE(TAG, "JPEG encoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (input == NULL || output == NULL || output_size == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    struct v4l2_buffer out_buf;
    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_buf.memory    = V4L2_MEMORY_USERPTR;
    out_buf.index     = 0;
    out_buf.m.userptr = (unsigned long)input;
    out_buf.length    = s_width * s_height * 2;

    if (ioctl(s_jpeg_fd, VIDIOC_QBUF, &out_buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to queue input buffer");
        return ESP_FAIL;
    }

    struct v4l2_buffer cap_buf;
    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_jpeg_fd, VIDIOC_DQBUF, &cap_buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to dequeue capture buffer");
        return ESP_FAIL;
    }

    *output_size = cap_buf.bytesused;
    if (*output_size > JPEG_OUTPUT_BUFFER_SIZE)
    {
        ESP_LOGW(TAG, "JPEG output too large: %d bytes", *output_size);
        *output_size = JPEG_OUTPUT_BUFFER_SIZE;
    }

    memcpy(output, s_output_buffer, *output_size);

    if (ioctl(s_jpeg_fd, VIDIOC_QBUF, &cap_buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to re-queue capture buffer");
        return ESP_FAIL;
    }

    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_buf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(s_jpeg_fd, VIDIOC_DQBUF, &out_buf) != 0)
    {
        ESP_LOGE(TAG, "Failed to dequeue output buffer");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t jpeg_encoder_deinit(void)
{
    if (s_jpeg_fd < 0)
    {
        ESP_LOGW(TAG, "JPEG encoder not initialized");
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_jpeg_fd, VIDIOC_STREAMOFF, &type);

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(s_jpeg_fd, VIDIOC_STREAMOFF, &type);

    if (s_output_buffer != NULL)
    {
        munmap(s_output_buffer, s_output_buffer_size);
        s_output_buffer      = NULL;
        s_output_buffer_size = 0;
    }

    close(s_jpeg_fd);
    s_jpeg_fd = -1;

    ESP_LOGI(TAG, "JPEG encoder deinitialized");
    return ESP_OK;
}

bool jpeg_encoder_is_ready(void)
{
    return s_jpeg_fd >= 0;
}

uint32_t jpeg_encoder_get_width(void)
{
    return s_width;
}

uint32_t jpeg_encoder_get_height(void)
{
    return s_height;
}

#endif
