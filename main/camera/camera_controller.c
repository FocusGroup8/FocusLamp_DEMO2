/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_EXAMPLE_ENABLE_CAMERA

#include "board_config.h"
#include "board_init.h"
#include "camera_controller.h"
#include "driver/isp.h"
#include "driver/isp_ae.h"
#include "driver/isp_hist.h"
#include "driver/isp_wbg.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ov5647.h"

#include <string.h>

static const char *TAG = "CAMERA_CTLR";

/* MIPI CSI PHY LDO — ESP32-P4 shares LDO channel 3 (2.5V) between DSI and CSI PHYs.
 * If the display system already acquired this channel, esp_ldo_acquire_channel
 * returns the existing handle (reference counted). */
static esp_ldo_channel_handle_t s_ldo_mipi_phy = NULL;

/* ----------------------------------------------------------------------- */
/* CSI Callbacks                                                            */
/*                                                                          */
/* NOTE: When on_get_new_trans is registered, the CSI DMA ISR always uses   */
/* it to get the next buffer, NEVER reading from the trans_que.             */
/* esp_cam_ctlr_receive() only writes to trans_que. These are ALTERNATIVE   */
/* mechanisms — mixing them causes the queue to fill up and block forever.  */
/*                                                                          */
/* Our approach: use on_get_new_trans for buffer provisioning, and          */
/* on_trans_finished + FreeRTOS task notification for frame completion      */
/* signaling. Do NOT call esp_cam_ctlr_receive().                           */
/* ----------------------------------------------------------------------- */

static bool s_camera_on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    camera_handles_t *handles = (camera_handles_t *)user_data;
    trans->buffer             = handles->csi_trans.buffer;
    trans->buflen             = handles->csi_trans.buflen;
    return false;
}

static bool s_camera_on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    camera_handles_t *handles  = (camera_handles_t *)user_data;
    BaseType_t high_task_woken = pdFALSE;
    if (handles->capture_task) {
        vTaskNotifyGiveFromISR((TaskHandle_t)handles->capture_task, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

/* ----------------------------------------------------------------------- */
/* Public API                                                               */
/* ----------------------------------------------------------------------- */

esp_err_t camera_controller_init(const camera_config_t *config, camera_handles_t *handles)
{
    esp_err_t ret        = ESP_FAIL;
    bool sccb_created    = false;
    bool sensor_detected = false;
    bool csi_created     = false;
    bool csi_enabled     = false;
    bool isp_created     = false;
    bool isp_enabled     = false;

    if (!config || !handles) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    memset(handles, 0, sizeof(camera_handles_t));
    handles->frame_buffer_size = config->h_res * config->v_res * BOARD_CAM_RGB565_BPP;

    /*--- Step 0: Power on MIPI PHY via LDO ---*/
    if (s_ldo_mipi_phy == NULL) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = BOARD_DSI_PHY_LDO_CHAN,
            .voltage_mv = BOARD_DSI_PHY_LDO_VOLTAGE_MV,
        };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_mipi_phy);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to acquire MIPI PHY LDO (chan %d): %s", BOARD_DSI_PHY_LDO_CHAN, esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "MIPI PHY powered on (LDO chan %d, %dmV)", BOARD_DSI_PHY_LDO_CHAN, BOARD_DSI_PHY_LDO_VOLTAGE_MV);
    }

    /*--- Step 1: Acquire shared I2C0 bus (increments ref count) ---*/
    ESP_LOGI(TAG, "Acquiring shared I2C0 bus...");
    i2c_master_bus_handle_t i2c_bus = NULL;
    ret                             = board_i2c_bus_init(&i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init shared I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /*--- Step 2: Create SCCB IO on shared bus ---*/
    ESP_LOGI(TAG, "Creating SCCB IO (addr=0x%02X, freq=%dHz)...", BOARD_CAM_SCCB_ADDR, BOARD_CAM_SCCB_FREQ_HZ);
    sccb_i2c_config_t sccb_i2c_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_CAM_SCCB_ADDR,
        .scl_speed_hz    = BOARD_CAM_SCCB_FREQ_HZ,
    };
    ret = sccb_new_i2c_io(i2c_bus, &sccb_i2c_config, &handles->sccb_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SCCB IO: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    sccb_created = true;

    /*--- Step 3: Detect OV5647 sensor ---*/
    ESP_LOGI(TAG, "Detecting OV5647 sensor...");
    esp_cam_sensor_config_t cam_sensor_config = {
        .sccb_handle = handles->sccb_handle,
        .reset_pin   = BOARD_CAM_RESET_GPIO,
        .pwdn_pin    = BOARD_CAM_PWDN_GPIO,
        .xclk_pin    = BOARD_CAM_XCLK_GPIO,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    handles->cam_sensor = ov5647_detect(&cam_sensor_config);
    if (handles->cam_sensor == NULL) {
        ESP_LOGE(TAG, "OV5647 sensor not detected");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    sensor_detected = true;
    ESP_LOGI(TAG, "OV5647 detected: %s", esp_cam_sensor_get_name(handles->cam_sensor));

    /*--- Step 4: Configure sensor output format ---*/
    ESP_LOGI(TAG, "Querying sensor formats...");
    esp_cam_sensor_format_array_t fmt_array = {0};
    ret                                     = esp_cam_sensor_query_format(handles->cam_sensor, &fmt_array);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query sensor formats: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    const esp_cam_sensor_format_t *target_fmt = NULL;
    for (int i = 0; i < (int)fmt_array.count; i++) {
        ESP_LOGI(TAG, "  fmt[%d]: %s", i, fmt_array.format_array[i].name);
        if (strcmp(fmt_array.format_array[i].name, config->format_name) == 0) {
            target_fmt = &fmt_array.format_array[i];
        }
    }

    if (target_fmt == NULL) {
        ESP_LOGE(TAG, "Format '%s' not supported by sensor", config->format_name);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    ret = esp_cam_sensor_set_format(handles->cam_sensor, target_fmt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set sensor format: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Sensor format set: %s", target_fmt->name);

    /*--- Step 5: Create CSI controller ---*/
    ESP_LOGI(TAG, "Creating CSI controller (ctlr=%d, %lux%lu, %d-lane, %dMbps)...", BOARD_CSI_CTLR_ID,
             (unsigned long)config->h_res, (unsigned long)config->v_res, BOARD_CSI_DATA_LANE_NUM,
             BOARD_CSI_LANE_BITRATE_MBPS);
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id                = BOARD_CSI_CTLR_ID,
        .h_res                  = config->h_res,
        .v_res                  = config->v_res,
        .lane_bit_rate_mbps     = BOARD_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = BOARD_CSI_DATA_LANE_NUM,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, &handles->csi_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CSI controller: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    csi_created = true;

    /*--- Step 6: Allocate frame buffer and register CSI callbacks ---*/
    ESP_LOGI(TAG, "Allocating frame buffer (%zu bytes)...", handles->frame_buffer_size);

    /* Get cache alignment requirement for PSRAM DMA buffers */
    size_t cache_alignment = 1;
    ret                    = esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &cache_alignment);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get cache alignment, using default: %s", esp_err_to_name(ret));
        cache_alignment = 64;
    }
    size_t alignment = cache_alignment > 4 ? cache_alignment : 4; /* At least 4 for DMA */
    ESP_LOGI(TAG, "Cache alignment: %zu, using alignment: %zu", cache_alignment, alignment);

    handles->frame_buffer =
        heap_caps_aligned_alloc(alignment, handles->frame_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (handles->frame_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%zu bytes)", handles->frame_buffer_size);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    /* Zero and sync the buffer before use */
    memset(handles->frame_buffer, 0, handles->frame_buffer_size);
    esp_cache_msync(handles->frame_buffer, handles->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Store trans in handles (persistent storage for callback user_data) */
    handles->csi_trans.buffer = handles->frame_buffer;
    handles->csi_trans.buflen = handles->frame_buffer_size;

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = s_camera_on_get_new_trans,
        .on_trans_finished = s_camera_on_trans_finished,
    };
    ret = esp_cam_ctlr_register_event_callbacks(handles->csi_ctlr, &cbs, handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register CSI callbacks: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_cam_ctlr_enable(handles->csi_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable CSI controller: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    csi_enabled = true;

    /*--- Step 7: Create ISP processor ---*/
    ESP_LOGI(TAG, "Creating ISP processor (clk=%dMHz, RAW8->RGB565, bayer=GBRG)...", BOARD_ISP_CLK_HZ / 1000000);
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz                 = BOARD_ISP_CLK_HZ,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = config->h_res,
        .v_res                  = config->v_res,
        /* OV5647 outputs Bayer RAW8 in GBRG order (see ov5647.c ov5647_isp_info[]).
         * Without this field, ISP defaults to BGGR, causing R/B channel swap and
         * green-tinted images. V4L2 driver path sets this automatically from
         * sensor_info->isp_v1_info.bayer_type, but the low-level API path requires
         * explicit configuration. */
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
    };
    ret = esp_isp_new_processor(&isp_config, &handles->isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ISP processor: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    isp_created = true;

    ret = esp_isp_enable(handles->isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable ISP processor: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    isp_enabled = true;

    /* Enable ISP Demosaic submodule: without demosaic, ISP passes Bayer RAW8
     * data through as-if it were RGB565, producing green-tinted, monochrome-like
     * images. V4L2 driver path enables this automatically (see esp_video_isp_device.c
     * isp_start_demosaic), but the low-level API path requires explicit enable.
     * Default config (grad_ratio=0) matches V4L2 default behavior. */
    esp_isp_demosaic_config_t demosaic_cfg = {0};
    ret                                    = esp_isp_demosaic_configure(handles->isp_proc, &demosaic_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP demosaic configure failed: %s", esp_err_to_name(ret));
    } else {
        ret = esp_isp_demosaic_enable(handles->isp_proc);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ISP demosaic enable failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "ISP demosaic enabled (bayer=GBRG)");
        }
    }

    /* Enable ISP Color submodule for basic contrast/saturation/hue/brightness.
     * Default values: contrast=1.0, saturation=1.0, hue=0, brightness=0. */
    esp_isp_color_config_t color_cfg = {
        .color_contrast   = {.integer = 1, .decimal = 0},
        .color_saturation = {.integer = 1, .decimal = 0},
        .color_hue        = 0,
        .color_brightness = 0,
    };
    ret = esp_isp_color_configure(handles->isp_proc, &color_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP color configure failed: %s", esp_err_to_name(ret));
    } else {
        ret = esp_isp_color_enable(handles->isp_proc);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ISP color enable failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "ISP color enabled");
        }
    }

    /* Enable ISP WBG (White Balance Gain) submodule.
     *
     * V4L2 driver path unconditionally enables WBG (see esp_video_isp_device.c
     * isp_start_pipeline), but uses red_balance_gain/blue_balance_gain from
     * static zero-initialized struct, which results in gain_r=0 and gain_b=0,
     * effectively zeroing R/B channels. This is a V4L2 driver bug or it
     * expects user-space to set gains via VIDIOC_S_EXT_CTRLS.
     *
     * Here we explicitly set gains to 1.0 (neutral) to preserve image colors
     * while still keeping the WBG submodule enabled for future tuning. */
    esp_isp_wbg_config_t wbg_cfg = {0};
    ret                          = esp_isp_wbg_configure(handles->isp_proc, &wbg_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP WBG configure failed: %s", esp_err_to_name(ret));
    } else {
        ret = esp_isp_wbg_enable(handles->isp_proc);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ISP WBG enable failed: %s", esp_err_to_name(ret));
        } else {
            /* Set neutral gains (1.0 for all channels) to avoid V4L2 default
             * zero-gain issue. ESP_VIDEO_ISP_WBG_DEC_BITS is the internal
             * fixed-point decimal bits used by the V4L2 driver; we use the
             * same value for consistency. */
#define ISP_WBG_DEC_BITS 8
            isp_wbg_gain_t wbg_gain = {
                .gain_r = (1 << ISP_WBG_DEC_BITS),
                .gain_g = (1 << ISP_WBG_DEC_BITS),
                .gain_b = (1 << ISP_WBG_DEC_BITS),
            };
            ret = esp_isp_wbg_set_wb_gain(handles->isp_proc, wbg_gain);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "ISP WBG set gain failed: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "ISP WBG enabled (gains R=G=B=1.0)");
            }
        }
    }

    /* Enable ISP AE (Auto Exposure) statistics controller.
     *
     * V4L2 driver path unconditionally starts AE controller with sample
     * point AFTER_DEMOSAIC and full-frame window (see isp_start_ae).
     * Statistics are collected but not consumed here (no callback registered),
     * matching V4L2 default behavior where user-space reads stats via
     * VIDIOC_DQBUF on the metadata device. Kept for pipeline parity and
     * future extension (e.g. auto-exposure control loop). */
    esp_isp_ae_config_t ae_cfg = {
        .sample_point = ISP_AE_SAMPLE_POINT_AFTER_DEMOSAIC,
        .window =
            {
                .top_left  = {0, 0},
                .btm_right = {config->h_res - 1, config->v_res - 1},
            },
        .intr_priority = 0,
    };
    ret = esp_isp_new_ae_controller(handles->isp_proc, &ae_cfg, &handles->ae_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP AE new controller failed: %s", esp_err_to_name(ret));
        handles->ae_ctlr = NULL;
    } else {
        ret = esp_isp_ae_controller_enable(handles->ae_ctlr);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ISP AE enable failed: %s", esp_err_to_name(ret));
            esp_isp_del_ae_controller(handles->ae_ctlr);
            handles->ae_ctlr = NULL;
        } else {
            ret = esp_isp_ae_controller_start_continuous_statistics(handles->ae_ctlr);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "ISP AE start continuous stats failed: %s", esp_err_to_name(ret));
                esp_isp_ae_controller_disable(handles->ae_ctlr);
                esp_isp_del_ae_controller(handles->ae_ctlr);
                handles->ae_ctlr = NULL;
            } else {
                ESP_LOGI(TAG, "ISP AE controller enabled (continuous stats, no callback)");
            }
        }
    }

    /* Enable ISP HIST (Histogram) statistics controller.
     *
     * V4L2 driver path unconditionally starts HIST controller in YUV_Y mode
     * with 25 subwindow weights and 16-segment thresholds (see isp_start_hist).
     * Default configuration copied from V4L2 driver isp_start_hist(). */
    esp_isp_hist_config_t hist_cfg = {
        .hist_mode = ISP_HIST_SAMPLING_YUV_Y,
        .rgb_coefficient =
            {
                .coeff_b = {{85, 0}},
                .coeff_g = {{85, 0}},
                .coeff_r = {{85, 0}},
            },
        .window_weight =
            {
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0},
                {.decimal = 12, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
                {.decimal = 10, .integer = 0},
            },
        .segment_threshold = {16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240},
        .window =
            {
                .top_left  = {0, 0},
                .btm_right = {config->h_res - 1, config->v_res - 1},
            },
    };
    ret = esp_isp_new_hist_controller(handles->isp_proc, &hist_cfg, &handles->hist_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP HIST new controller failed: %s", esp_err_to_name(ret));
        handles->hist_ctlr = NULL;
    } else {
        /* HIST controller enable and continuous statistics start are deferred to
         * camera_stream_start() to allow callback registration while controller
         * is in init state (ESP-IDF API requirement: register before enable).
         * Callback registration requires fsm==INIT, but enable() transitions
         * fsm to ENABLE, so we must not enable here. */
        ESP_LOGI(TAG, "ISP HIST controller created (YUV_Y mode, enable+start deferred)");
    }

    /*--- Step 8: Create JPEG hardware encoder ---*/
    ESP_LOGI(TAG, "Creating JPEG encoder (RGB565->JPEG, Q=%d, subsampling=%s)...", BOARD_JPEG_QUALITY,
             BOARD_JPEG_SUB_SAMPLE == JPEG_DOWN_SAMPLING_YUV420   ? "YUV420"
             : BOARD_JPEG_SUB_SAMPLE == JPEG_DOWN_SAMPLING_YUV422 ? "YUV422"
             : BOARD_JPEG_SUB_SAMPLE == JPEG_DOWN_SAMPLING_YUV444 ? "YUV444"
                                                                  : "UNKNOWN");
    jpeg_encode_engine_cfg_t jpeg_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 100,
    };
    ret = jpeg_new_encoder_engine(&jpeg_eng_cfg, &handles->jpeg_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "JPEG encoder created");

    /* Allocate JPEG output buffer in PSRAM */
    jpeg_encode_memory_alloc_cfg_t jpeg_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    handles->jpeg_out_buf = jpeg_alloc_encoder_mem(BOARD_JPEG_OUT_BUF_SIZE, &jpeg_mem_cfg, &handles->jpeg_out_buf_size);
    if (handles->jpeg_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer (%d bytes)", BOARD_JPEG_OUT_BUF_SIZE);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "JPEG output buffer allocated: %zu bytes (requested %d)", handles->jpeg_out_buf_size,
             BOARD_JPEG_OUT_BUF_SIZE);

    handles->is_initialized = true;
    ESP_LOGI(TAG, "Camera pipeline initialized successfully");
    return ESP_OK;

cleanup:
    /* Release resources in reverse order of creation */
    if (handles->jpeg_out_buf) {
        free(handles->jpeg_out_buf);
        handles->jpeg_out_buf = NULL;
    }
    if (handles->jpeg_encoder) {
        jpeg_del_encoder_engine(handles->jpeg_encoder);
        handles->jpeg_encoder = NULL;
    }
    /* Stop and delete AE/HIST statistics controllers (created after Color) */
    if (handles->hist_ctlr) {
        esp_isp_hist_controller_stop_continuous_statistics(handles->hist_ctlr);
        esp_isp_hist_controller_disable(handles->hist_ctlr);
        esp_isp_del_hist_controller(handles->hist_ctlr);
        handles->hist_ctlr = NULL;
    }
    if (handles->ae_ctlr) {
        esp_isp_ae_controller_stop_continuous_statistics(handles->ae_ctlr);
        esp_isp_ae_controller_disable(handles->ae_ctlr);
        esp_isp_del_ae_controller(handles->ae_ctlr);
        handles->ae_ctlr = NULL;
    }
    if (isp_enabled) {
        esp_isp_disable(handles->isp_proc);
    }
    if (isp_created) {
        esp_isp_del_processor(handles->isp_proc);
        handles->isp_proc = NULL;
    }
    if (csi_enabled) {
        esp_cam_ctlr_disable(handles->csi_ctlr);
    }
    if (csi_created) {
        esp_cam_ctlr_del(handles->csi_ctlr);
        handles->csi_ctlr = NULL;
    }
    /* Free frame buffer manually (allocated via heap_caps_aligned_alloc) */
    if (handles->frame_buffer) {
        heap_caps_free(handles->frame_buffer);
        handles->frame_buffer = NULL;
    }
    if (sensor_detected) {
        esp_cam_sensor_del_dev(handles->cam_sensor);
        handles->cam_sensor = NULL;
    }
    if (sccb_created) {
        esp_sccb_del_i2c_io(handles->sccb_handle);
        handles->sccb_handle = NULL;
    }
    handles->is_initialized = false;
    return ret;
}

esp_err_t camera_controller_start(camera_handles_t *handles)
{
    if (!handles || !handles->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (handles->is_streaming) {
        ESP_LOGW(TAG, "Camera already streaming");
        return ESP_OK;
    }

    esp_err_t ret;

    /* Start CSI controller first */
    ESP_LOGI(TAG, "Starting CSI controller...");
    ret = esp_cam_ctlr_start(handles->csi_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start CSI controller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "CSI controller started");

    /* Start sensor output stream */
    ESP_LOGI(TAG, "Starting sensor stream...");
    int enable = 1;
    ret        = esp_cam_sensor_ioctl(handles->cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &enable);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sensor stream: %s", esp_err_to_name(ret));
        esp_cam_ctlr_stop(handles->csi_ctlr);
        return ret;
    }

    handles->is_streaming = true;
    ESP_LOGI(TAG, "Camera streaming started");
    return ESP_OK;
}

esp_err_t camera_controller_stop(camera_handles_t *handles)
{
    if (!handles || !handles->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!handles->is_streaming) {
        ESP_LOGW(TAG, "Camera not streaming");
        return ESP_OK;
    }

    esp_err_t ret;

    /* Stop sensor stream first */
    int disable = 0;
    ret         = esp_cam_sensor_ioctl(handles->cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &disable);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop sensor stream: %s", esp_err_to_name(ret));
    }

    /* Stop CSI controller */
    ret = esp_cam_ctlr_stop(handles->csi_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop CSI controller: %s", esp_err_to_name(ret));
    }

    handles->is_streaming = false;
    ESP_LOGI(TAG, "Camera streaming stopped");
    return ESP_OK;
}

esp_err_t camera_controller_deinit(camera_handles_t *handles)
{
    if (!handles || !handles->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop streaming if active */
    if (handles->is_streaming) {
        camera_controller_stop(handles);
    }

    /* Delete JPEG encoder and free output buffer */
    if (handles->jpeg_encoder) {
        jpeg_del_encoder_engine(handles->jpeg_encoder);
        handles->jpeg_encoder = NULL;
    }
    if (handles->jpeg_out_buf) {
        free(handles->jpeg_out_buf);
        handles->jpeg_out_buf = NULL;
    }

    /* Stop and delete AE/HIST statistics controllers (created after Color) */
    if (handles->hist_ctlr) {
        esp_isp_hist_controller_stop_continuous_statistics(handles->hist_ctlr);
        esp_isp_hist_controller_disable(handles->hist_ctlr);
        esp_isp_del_hist_controller(handles->hist_ctlr);
        handles->hist_ctlr = NULL;
    }
    if (handles->ae_ctlr) {
        esp_isp_ae_controller_stop_continuous_statistics(handles->ae_ctlr);
        esp_isp_ae_controller_disable(handles->ae_ctlr);
        esp_isp_del_ae_controller(handles->ae_ctlr);
        handles->ae_ctlr = NULL;
    }

    /* Disable and delete ISP */
    if (handles->isp_proc) {
        esp_isp_disable(handles->isp_proc);
        esp_isp_del_processor(handles->isp_proc);
        handles->isp_proc = NULL;
    }

    /* Disable and delete CSI controller */
    if (handles->csi_ctlr) {
        esp_cam_ctlr_disable(handles->csi_ctlr);
        esp_cam_ctlr_del(handles->csi_ctlr);
        handles->csi_ctlr = NULL;
    }

    /* Free frame buffer manually (allocated via heap_caps_aligned_alloc) */
    if (handles->frame_buffer) {
        heap_caps_free(handles->frame_buffer);
        handles->frame_buffer = NULL;
    }

    /* Delete sensor device */
    if (handles->cam_sensor) {
        esp_cam_sensor_del_dev(handles->cam_sensor);
        handles->cam_sensor = NULL;
    }

    /* Delete SCCB IO */
    if (handles->sccb_handle) {
        esp_sccb_del_i2c_io(handles->sccb_handle);
        handles->sccb_handle = NULL;
    }

    /* Release shared I2C bus reference */
    board_i2c_bus_deinit();

    handles->is_initialized    = false;
    handles->is_streaming      = false;
    handles->frame_buffer_size = 0;

    ESP_LOGI(TAG, "Camera pipeline deinitialized");
    return ESP_OK;
}

esp_err_t camera_capture_frame(camera_handles_t *handles)
{
    if (!handles || !handles->is_initialized || !handles->is_streaming) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Do NOT call esp_cam_ctlr_receive() here!
     *
     * The CSI driver has two alternative buffer provisioning mechanisms:
     *   1) on_get_new_trans callback (used by us) — ISR calls this directly
     *   2) trans_que (used by esp_cam_ctlr_receive) — ISR reads from queue
     *
     * When on_get_new_trans is registered, the ISR never reads from trans_que.
     * Calling esp_cam_ctlr_receive() would fill the queue with items that are
     * never consumed, eventually blocking forever (queue_items=1).
     *
     * Instead, we use FreeRTOS task notification: on_trans_finished (called
     * from DMA ISR) notifies this task, and we block here until a frame
     * arrives or timeout.
     */
    handles->capture_task = xTaskGetCurrentTaskHandle();

    /* Clear any pending notifications from previous frames */
    ulTaskNotifyTake(pdTRUE, 0);

    /* Wait for the next frame completion signal from on_trans_finished */
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    if (notified == 0) {
        ESP_LOGE(TAG, "capture_frame: timeout waiting for frame (5s)");
        return ESP_ERR_TIMEOUT;
    }

    /* Cache invalidate: ensure CPU sees the latest DMA-written data */
    esp_err_t ret = esp_cache_msync(handles->frame_buffer, handles->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "capture_frame: cache msync failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGD(TAG, "capture_frame: frame received (buf=%p, size=%zu)", handles->frame_buffer,
             handles->frame_buffer_size);
    return ESP_OK;
}

esp_err_t camera_encode_jpeg(camera_handles_t *handles, int quality, uint32_t *out_size)
{
    if (!handles || !handles->is_initialized || !handles->jpeg_encoder || !handles->jpeg_out_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!handles->frame_buffer) {
        ESP_LOGE(TAG, "No frame buffer available for JPEG encoding");
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Cache invalidate: ensure JPEG encoder sees latest DMA-written data */
    esp_err_t ret = esp_cache_msync(handles->frame_buffer, handles->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG input cache msync failed: %s", esp_err_to_name(ret));
    }

    jpeg_encode_cfg_t encode_cfg = {
        .height        = BOARD_CAM_V_RES,
        .width         = BOARD_CAM_H_RES,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = BOARD_JPEG_SUB_SAMPLE,
        .image_quality = quality,
    };

    ret = jpeg_encoder_process(handles->jpeg_encoder, &encode_cfg, (const uint8_t *)handles->frame_buffer,
                               (uint32_t)handles->frame_buffer_size, handles->jpeg_out_buf,
                               (uint32_t)handles->jpeg_out_buf_size, out_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* High-frequency log: downgrade to DEBUG to avoid flooding serial console */
    ESP_LOGD(TAG, "JPEG encoded: %lu bytes (quality=%d, subsample=YUV422)", (unsigned long)*out_size, quality);
    return ESP_OK;
}

const uint8_t *camera_get_jpeg_buffer(camera_handles_t *handles)
{
    if (!handles || !handles->jpeg_out_buf) {
        return NULL;
    }
    return handles->jpeg_out_buf;
}

#endif /* CONFIG_EXAMPLE_ENABLE_CAMERA */
