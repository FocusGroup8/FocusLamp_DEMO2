/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_EXAMPLE_ENABLE_CAMERA

#include "board_config.h"
#include "camera_preview.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "hal/ppa_types.h"

#include <string.h>

static const char *TAG = "CAM_PREVIEW";

/* Preview dimensions: scale full 800x640 frame to 400x320 (0.5x, no crop)
 * Diagnostic mode: simplified scaling to isolate byte-order issues.
 */
#define PREVIEW_CROP_W 800   /* Full frame width (no crop) */
#define PREVIEW_CROP_H 640   /* Full frame height (no crop) */
#define PREVIEW_CROP_X 0     /* No horizontal offset */
#define PREVIEW_CROP_Y 0     /* No vertical offset */
#define PREVIEW_OUT_W 400    /* Output width (scaled down 0.5x) */
#define PREVIEW_OUT_H 320    /* Output height (scaled down 0.5x) */
#define PREVIEW_RGB565_BPP 2 /* Bytes per pixel for RGB565 */

esp_err_t camera_preview_init(camera_preview_t *preview)
{
    if (!preview) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(preview, 0, sizeof(camera_preview_t));

    /* Step 1: Register PPA SRM client */
    ESP_LOGI(TAG, "Registering PPA SRM client...");
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };
    esp_err_t ret = ppa_register_client(&ppa_cfg, &preview->ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PPA SRM client registered");

    /* Step 2: Allocate RGB565 output buffer in PSRAM (also used as LVGL canvas buffer).
     * MALLOC_CAP_DMA is required because PPA writes this buffer via 2D-DMA.
     * Official ESP-BSP lcd_ppa.c and ESP-IDF ppa_dsi example both use MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM. */
    preview->out_buf_size = PREVIEW_OUT_W * PREVIEW_OUT_H * PREVIEW_RGB565_BPP;
    ESP_LOGI(TAG, "Allocating output buffer (%u bytes, PSRAM+DMA)...", (unsigned)preview->out_buf_size);

    preview->out_buf = heap_caps_aligned_calloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, preview->out_buf_size, sizeof(uint8_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (preview->out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%u bytes)", (unsigned)preview->out_buf_size);
        ppa_unregister_client(preview->ppa_client);
        preview->ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Output buffer allocated at %p", preview->out_buf);

    /* Step 3: Allocate PPA input buffer in PSRAM (copy of camera frame, isolates ISP DMA from PPA DMA).
     * DMA-capable is required because PPA reads this buffer via 2D-DMA. */
    preview->ppa_input_buf_size = BOARD_CAM_H_RES * BOARD_CAM_V_RES * PREVIEW_RGB565_BPP;
    ESP_LOGI(TAG, "Allocating PPA input buffer (%u bytes, PSRAM+DMA)...", (unsigned)preview->ppa_input_buf_size);

    preview->ppa_input_buf = heap_caps_aligned_calloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, preview->ppa_input_buf_size,
                                                      sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (preview->ppa_input_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PPA input buffer (%u bytes)", (unsigned)preview->ppa_input_buf_size);
        heap_caps_free(preview->out_buf);
        preview->out_buf = NULL;
        ppa_unregister_client(preview->ppa_client);
        preview->ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PPA input buffer allocated at %p", preview->ppa_input_buf);

    preview->is_initialized = true;
    ESP_LOGI(TAG, "Camera preview initialized (scale %dx%d -> %dx%d RGB565, no crop)", PREVIEW_CROP_W, PREVIEW_CROP_H,
             PREVIEW_OUT_W, PREVIEW_OUT_H);
    return ESP_OK;
}

esp_err_t camera_preview_process_frame(const camera_handles_t *handles, camera_preview_t *preview)
{
    if (!handles || !preview || !preview->is_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handles->frame_buffer) {
        ESP_LOGE(TAG, "No camera frame buffer available");
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 1: Cache invalidate (M2C) — discard stale CPU cache so CPU reads fresh
     * DMA-written data from PSRAM.
     * Note: M2C direction does NOT allow ESP_CACHE_MSYNC_FLAG_UNALIGNED in ESP-IDF v5.5.4.
     * frame_buffer is allocated with cache-line alignment, so plain M2C is safe. */
    esp_err_t ret = esp_cache_msync(handles->frame_buffer, handles->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cache msync (M2C on frame_buffer) failed: %s", esp_err_to_name(ret));
    }

    /* Diagnostic: dump first 16 bytes of ISP output (first 8 pixels of RGB565) */
    {
        uint8_t *raw = (uint8_t *)handles->frame_buffer;
        ESP_LOGD(
            TAG,
            "ISP raw bytes [0..15]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12],
            raw[13], raw[14], raw[15]);
        /* Also dump bytes at start of row 1 (offset = 800*2 = 1600) */
        uint8_t *row1 = raw + 1600;
        ESP_LOGD(TAG, "ISP row1 bytes [0..7]: %02X %02X %02X %02X %02X %02X %02X %02X", row1[0], row1[1], row1[2],
                 row1[3], row1[4], row1[5], row1[6], row1[7]);
    }

    /* Step 2: 方案C — Buffer copy isolation.
     * Copy frame_buffer (ISP DMA target) to ppa_input_buf (CPU-owned, DMA-capable).
     * This completely isolates PPA's 2D-DMA read from ISP's DMA write, eliminating
     * any cache coherency interaction between the two DMA engines.
     *
     * After memcpy, CPU cache holds fresh data; we must C2M (write-back) to push
     * it to PSRAM so PPA's DMA reads the updated bytes. PPA driver's internal
     * C2M on the input buffer then becomes a no-op (cache already clean). */
    memcpy(preview->ppa_input_buf, handles->frame_buffer, handles->frame_buffer_size);

    ret = esp_cache_msync(preview->ppa_input_buf, preview->ppa_input_buf_size,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cache msync (C2M on ppa_input_buf) failed: %s", esp_err_to_name(ret));
    }

    /* Step 3: PPA SRM scaling (RGB565→RGB565, same color mode).
     * Configuration: 800x640 full frame → 400x320 output (0.5x downscale, no crop).
     * rgb_swap=false, byte_swap=false (PPA expects little-endian RGB565). */
    ppa_srm_oper_config_t srm_cfg = {
        .in =
            {
                .buffer         = preview->ppa_input_buf,
                .pic_w          = BOARD_CAM_H_RES, /* 800 */
                .pic_h          = BOARD_CAM_V_RES, /* 640 */
                .block_w        = PREVIEW_CROP_W,  /* 800 (full frame, no crop) */
                .block_h        = PREVIEW_CROP_H,  /* 640 (full frame, no crop) */
                .block_offset_x = PREVIEW_CROP_X,  /* 0 */
                .block_offset_y = PREVIEW_CROP_Y,  /* 0 */
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
        .out =
            {
                .buffer         = preview->out_buf,
                .buffer_size    = preview->out_buf_size,
                .pic_w          = PREVIEW_OUT_W, /* 400 */
                .pic_h          = PREVIEW_OUT_H, /* 320 */
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = (float)PREVIEW_OUT_W / PREVIEW_CROP_W, /* 0.5 */
        .scale_y           = (float)PREVIEW_OUT_H / PREVIEW_CROP_H, /* 0.5 */
        .mirror_x          = false,
        .mirror_y          = false,
        .rgb_swap          = false,
        .byte_swap         = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode              = PPA_TRANS_MODE_BLOCKING,
        .user_data         = NULL,
    };

    ret = ppa_do_scale_rotate_mirror(preview->ppa_client, &srm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM operation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 4: Cache invalidate on output buffer — ensure CPU sees PPA's DMA-written data.
     * Note: M2C direction does NOT allow ESP_CACHE_MSYNC_FLAG_UNALIGNED in ESP-IDF v5.5.4.
     * out_buf is allocated with cache-line alignment, so plain M2C is safe. */
    ret = esp_cache_msync(preview->out_buf, preview->out_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cache msync (M2C on PPA output) failed: %s", esp_err_to_name(ret));
    }

    /* Diagnostic: dump first 16 bytes of PPA output (first 8 pixels) */
    {
        uint8_t *out = preview->out_buf;
        ESP_LOGD(
            TAG,
            "PPA out bytes [0..15]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11], out[12],
            out[13], out[14], out[15]);
    }

    return ESP_OK;
}

uint8_t *camera_preview_get_buffer(const camera_preview_t *preview)
{
    if (!preview || !preview->is_initialized) {
        return NULL;
    }
    return preview->out_buf;
}

esp_err_t camera_preview_deinit(camera_preview_t *preview)
{
    if (!preview) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!preview->is_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing camera preview...");

    if (preview->ppa_input_buf) {
        heap_caps_free(preview->ppa_input_buf);
        preview->ppa_input_buf = NULL;
    }
    preview->ppa_input_buf_size = 0;

    if (preview->out_buf) {
        heap_caps_free(preview->out_buf);
        preview->out_buf = NULL;
    }
    if (preview->ppa_client) {
        ppa_unregister_client(preview->ppa_client);
        preview->ppa_client = NULL;
    }

    preview->out_buf_size   = 0;
    preview->is_initialized = false;

    ESP_LOGI(TAG, "Camera preview deinitialized");
    return ESP_OK;
}

#endif /* CONFIG_EXAMPLE_ENABLE_CAMERA */
