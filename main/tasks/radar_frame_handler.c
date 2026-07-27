/*
 * radar_frame_handler.c - 雷达帧处理实现
 *
 * 根据帧 message_type 解码数据，并通过 event_bus 发布 EV_RADAR_* 事件。
 * 同时集成 HRV 分析和运动分析。
 */

#include "radar_frame_handler.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"

#include "radar_module.h"
#include "radar_decoder.h"
#include "radar_breath_handler.h"
#include "hrv_analyzer.h"
#include "motion_analyzer.h"
#include "event_bus.h"
#include "event_def.h"

static const char *TAG = "radar_frame_handler";

static bool s_human_present = false;

void radar_frame_handler_init(void)
{
    radar_breath_handler_init();
    hrv_analyzer_init();
    motion_analyzer_init();
    s_human_present = false;
    ESP_LOGI(TAG, "Frame handler initialized");
}

void radar_frame_handler_handle(const struct tf_frame *frame, uint32_t elapsed_ms)
{
    if (frame == NULL) {
        return;
    }

    switch (frame->message_type) {
    case RADAR_MSG_TYPE_HUMAN_DETECT:
        if (frame->data_length >= 1U) {
            const bool present = radar_decoder_decode_bool(frame);
            s_human_present    = present;

            radar_presence_data_t pd = {
                .is_present  = present,
                .distance_cm = 0.0f,
                .x           = 0.0f,
                .y           = 0.0f,
                .z           = 0.0f,
            };

            event_t ev = {
                .type      = EV_RADAR_PRESENCE,
                .data      = &pd,
                .data_size = sizeof(pd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);

            /* Update motion analyzer */
            motion_analyzer_update_human_presence(present);
        }
        break;

    case RADAR_MSG_TYPE_PHASE_TEST:
        if (frame->data_length >= 12) {
            /* 偏移 4: 呼吸相位; 偏移 8: 心跳相位 */
            const float breath_phase = radar_decoder_decode_float_at(frame, 4);
            radar_breath_handler_update(breath_phase, elapsed_ms);

            if (radar_breath_handler_has_phase()) {
                radar_breath_data_t bd = {
                    .is_detected  = true,
                    .breath_phase = radar_breath_handler_get_filtered_phase(),
                    .breath_rate  = 0.0f,
                };
                event_t ev = {
                    .type      = EV_RADAR_BREATH,
                    .data      = &bd,
                    .data_size = sizeof(bd),
                    .timestamp = event_bus_get_timestamp(),
                };
                event_bus_publish(&ev);
            }

            /* 心跳相位用于 HRV */
            const float heart_phase = radar_decoder_decode_float_at(frame, 8);
            hrv_analyzer_feed_phase(heart_phase, (int64_t)elapsed_ms);

            if (hrv_analyzer_has_new_metrics()) {
                hrv_metrics_t metrics = {0};
                hrv_analyzer_get_metrics(&metrics);
                hrv_analyzer_clear_new_metrics_flag();

                radar_hrv_data_t hrv = {
                    .sdnn_ms    = metrics.sdnn,
                    .rmssd_ms   = metrics.rmssd,
                    .mean_hr_bpm = metrics.mean_hr,
                    .valid      = (metrics.valid_rr_count >= 2),
                };
                event_t ev = {
                    .type      = EV_RADAR_HRV_READY,
                    .data      = &hrv,
                    .data_size = sizeof(hrv),
                    .timestamp = event_bus_get_timestamp(),
                };
                event_bus_publish(&ev);

                ESP_LOGD(TAG, "HRV: SDNN=%.2f ms, RMSSD=%.2f ms, MeanHR=%.1f bpm",
                         metrics.sdnn, metrics.rmssd, metrics.mean_hr);
            }
        }
        break;

    case RADAR_MSG_TYPE_HEART_RATE:
        if (frame->data_length >= sizeof(float)) {
            const float hr = radar_decoder_decode_float_at(frame, 0);

            radar_heart_data_t hd = {
                .is_valid    = true,
                .heart_rate  = hr,
                .heart_phase = 0.0f,
            };
            event_t ev = {
                .type      = EV_RADAR_HEART_RATE,
                .data      = &hd,
                .data_size = sizeof(hd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;

    case RADAR_MSG_TYPE_BREATH_RATE:
        if (frame->data_length >= sizeof(float)) {
            const float br = radar_decoder_decode_float_at(frame, 0);

            radar_breath_data_t bd = {
                .is_detected  = true,
                .breath_phase = 0.0f,
                .breath_rate  = br,
            };
            event_t ev = {
                .type      = EV_RADAR_BREATH,
                .data      = &bd,
                .data_size = sizeof(bd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;

    case RADAR_MSG_TYPE_FIRMWARE_STATUS:
        if (frame->data_length >= 4) {
            const uint32_t firmware_version = radar_decoder_decode_uint32_at(frame, 0);
            ESP_LOGI(TAG, "Radar firmware version: 0x%08" PRIX32, firmware_version);
        }
        break;

    case RADAR_MSG_TYPE_POSITION:
        if (frame->data_length >= 8) {
            const uint32_t target_num = radar_decoder_decode_uint32_at(frame, 0);
            const size_t   req_size   = 4U + (size_t)target_num * 16U;

            if (frame->data_length >= req_size) {
                for (uint32_t i = 0; i < target_num; i++) {
                    const size_t  offset    = 4U + (size_t)i * 16U;
                    const float   x         = radar_decoder_decode_float_at(frame, offset);
                    const float   y         = radar_decoder_decode_float_at(frame, offset + 4);
                    const int32_t dop_idx   = radar_decoder_decode_int32_at(frame, offset + 8);
                    const int32_t cluster_id = radar_decoder_decode_int32_at(frame, offset + 12);

                    if (i == 0) {
                        radar_position_data_t pd = {
                            .x          = x,
                            .y          = y,
                            .z          = 0.0f,
                            .dop_idx    = dop_idx,
                            .cluster_id = cluster_id,
                        };
                        event_t ev = {
                            .type      = EV_RADAR_POSITION,
                            .data      = &pd,
                            .data_size = sizeof(pd),
                            .timestamp = event_bus_get_timestamp(),
                        };
                        event_bus_publish(&ev);

                        /* Update motion analyzer */
                        motion_analyzer_update_position(x, y, 0.0f, dop_idx, (int64_t)elapsed_ms);
                    }
                }
            }
        }
        break;

    case RADAR_MSG_TYPE_TARGET_RANGE:
        if (frame->data_length >= 8) {
            const uint32_t flag  = radar_decoder_decode_uint32_at(frame, 0);
            const float    range = radar_decoder_decode_float_at(frame, 4);

            radar_target_range_data_t rd = {
                .flag     = flag,
                .range_cm = range,
            };
            event_t ev = {
                .type      = EV_RADAR_TARGET_RANGE,
                .data      = &rd,
                .data_size = sizeof(rd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;

    case RADAR_MSG_TYPE_TARGET_TRACK:
        if (frame->data_length >= 12) {
            const float tx = radar_decoder_decode_float_at(frame, 0);
            const float ty = radar_decoder_decode_float_at(frame, 4);
            const float tz = radar_decoder_decode_float_at(frame, 8);

            radar_target_track_data_t td = {
                .x = tx,
                .y = ty,
                .z = tz,
            };
            event_t ev = {
                .type      = EV_RADAR_TARGET_TRACK,
                .data      = &td,
                .data_size = sizeof(td),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;

    default:
        ESP_LOGD(TAG, "Frame: Type=0x%04X, Len=%u",
                 frame->message_type, frame->data_length);
        break;
    }
}