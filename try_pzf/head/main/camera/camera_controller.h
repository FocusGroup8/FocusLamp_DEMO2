/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/isp.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor.h"
#include "esp_err.h"
#include "esp_sccb_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file camera_controller.h
 * @brief MIPI-CSI camera pipeline controller for ESP32-P4 + OV5647
 *
 * Manages the full camera pipeline: SCCB -> OV5647 Sensor -> CSI -> ISP
 * Input:  OV5647 RAW8 800x640 50fps (MIPI-CSI 2-lane)
 * Output: RGB565 via ISP conversion
 */

/**
 * @brief Camera pipeline handles
 */
typedef struct {
    esp_cam_sensor_device_t *cam_sensor; /*!< OV5647 sensor device handle */
    esp_cam_ctlr_handle_t csi_ctlr;      /*!< CSI controller handle */
    isp_proc_handle_t isp_proc;          /*!< ISP processor handle */
    isp_ae_ctlr_t ae_ctlr;               /*!< ISP AE controller (statistics, V4L2-aligned) */
    isp_hist_ctlr_t hist_ctlr;           /*!< ISP HIST controller (statistics, V4L2-aligned) */
    esp_sccb_io_handle_t sccb_handle;    /*!< SCCB I2C IO handle */
    void *frame_buffer;                  /*!< Frame buffer in PSRAM */
    size_t frame_buffer_size;            /*!< Frame buffer size in bytes */
    esp_cam_ctlr_trans_t csi_trans;      /*!< CSI transaction (persistent storage for callback user_data) */
    void *capture_waiters[2];            /*!< Streaming and optional local preview, protected against the CSI ISR */
    jpeg_encoder_handle_t jpeg_encoder;  /*!< JPEG hardware encoder handle */
    uint8_t *jpeg_out_buf;               /*!< JPEG output buffer in PSRAM */
    size_t jpeg_out_buf_size;            /*!< JPEG output buffer capacity in bytes */
    struct ppa_client_t *jpeg_scaler;
    void *jpeg_scale_input;
    void *jpeg_scale_output;
    int jpeg_width_requested;
    int jpeg_width_encoded;
    bool is_initialized;                 /*!< Initialization state flag */
    int exposure_target;                 /*!< Last successfully programmed sensor AE target */
    bool shadow_gamma;                   /*!< Hardware shadow-lift curve enabled */
    int gamma_level;                     /*!< 0 off, 1 mild, 2 medium, 3 strong */
    bool is_streaming;                   /*!< Streaming state flag */
} camera_handles_t;

/**
 * @brief Camera configuration parameters
 */
typedef struct {
    const char *format_name; /*!< Sensor output format name (e.g. "MIPI_2lane_24Minput_RAW8_800x640_50fps") */
    uint32_t h_res;          /*!< Horizontal resolution in pixels */
    uint32_t v_res;          /*!< Vertical resolution in pixels */
} camera_config_t;

/**
 * @brief Initialize the full camera pipeline (SCCB -> Sensor -> CSI -> ISP)
 *
 * Steps:
 * 1. Acquire shared I2C0 bus via board_i2c_bus_get_handle()
 * 2. Create SCCB IO on shared bus (OV5647 address 0x36, 10kHz)
 * 3. Detect OV5647 sensor via ov5647_detect()
 * 4. Configure sensor output format
 * 5. Create CSI controller (RAW8 input -> RGB565 output)
 * 6. Register CSI callbacks
 * 7. Create ISP processor (RAW8 -> RGB565, input from CSI)
 * 8. Allocate PSRAM frame buffer
 *
 * @param[in]  config   Camera configuration (format, resolution)
 * @param[out] handles  Camera pipeline handles (zeroed before use)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t camera_controller_init(const camera_config_t *config, camera_handles_t *handles);
esp_err_t camera_set_exposure_target(camera_handles_t *handles, int target);
esp_err_t camera_set_shadow_gamma(camera_handles_t *handles, bool enabled);
esp_err_t camera_set_gamma_level(camera_handles_t *handles, int level);

/**
 * @brief Start camera streaming
 *
 * Enables ISP, starts CSI controller, then starts sensor output stream.
 *
 * @param[in] handles  Camera pipeline handles
 * @return ESP_OK on success
 */
esp_err_t camera_controller_start(camera_handles_t *handles);

/**
 * @brief Stop camera streaming
 *
 * Stops sensor stream, CSI controller, and disables ISP.
 *
 * @param[in] handles  Camera pipeline handles
 * @return ESP_OK on success
 */
esp_err_t camera_controller_stop(camera_handles_t *handles);

/**
 * @brief Release all camera pipeline resources
 *
 * Stops streaming if active, then releases ISP, CSI, SCCB, and frame buffer.
 * Does NOT delete the shared I2C bus (managed by board_init).
 *
 * @param[in] handles  Camera pipeline handles
 * @return ESP_OK on success
 */
esp_err_t camera_controller_deinit(camera_handles_t *handles);

/**
 * @brief Capture one frame into the internal frame buffer
 *
 * Blocks until a frame is received or timeout.
 *
 * @param[in] handles  Camera pipeline handles
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t camera_capture_frame(camera_handles_t *handles);

/**
 * @brief Encode the current frame buffer to JPEG
 *
 * Takes the RGB565 frame already captured in handles->frame_buffer and
 * encodes it to JPEG using the ESP32-P4 hardware JPEG encoder.
 *
 * @param[in]  handles      Camera pipeline handles (must have a captured frame)
 * @param[in]  quality      JPEG quality (1-100, higher = better quality, larger size)
 * @param[out] out_size     Actual size of the JPEG output in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t camera_encode_jpeg(camera_handles_t *handles, int quality, uint32_t *out_size);
esp_err_t camera_set_jpeg_width(camera_handles_t *handles, int width);
int camera_get_jpeg_width(const camera_handles_t *handles, bool encoded);

/**
 * @brief Get the JPEG output buffer pointer
 *
 * Returns the pointer to the JPEG output buffer for reading the encoded data.
 * The buffer content is valid until the next call to camera_encode_jpeg().
 *
 * @param[in] handles  Camera pipeline handles
 * @return Pointer to JPEG output buffer, or NULL if not initialized
 */
const uint8_t *camera_get_jpeg_buffer(camera_handles_t *handles);

/**
 * @brief Run camera controller test
 *
 * Captures frames using an already-initialized camera pipeline.
 * Does NOT initialize or deinitialize the camera — uses the provided handles.
 *
 * @param[in] handles  Camera pipeline handles (must be initialized)
 * @return ESP_OK on success
 */
esp_err_t camera_test_run(camera_handles_t *handles);

#ifdef __cplusplus
}
#endif
