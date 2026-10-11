/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gesture_recognition.h"

#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <stdlib.h>

static const char *TAG = "GESTURE";

// Debug logging macro: controlled by Kconfig EXAMPLE_GESTURE_DEBUG_LOG
#if CONFIG_EXAMPLE_GESTURE_DEBUG_LOG
#define GESTURE_LOGD(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define GESTURE_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#endif

// Helper function to calculate distance between two points
static float calculate_distance(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

// Helper function to calculate angle between two points
static float calculate_angle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    int dx      = x2 - x1;
    int dy      = y2 - y1;
    float angle = atan2(dy, dx) * 180.0 / M_PI;
    return angle;
}

void gesture_recognition_init(gesture_recognition_t *gesture, void (*callback)(gesture_type_t, void *), void *user_data)
{
    // Set default detection thresholds (from board_config.h)
    gesture->swipe_threshold      = APP_GESTURE_SWIPE_THRESHOLD_PX;
    gesture->long_press_threshold = APP_GESTURE_LONG_PRESS_THRESHOLD_MS;
    gesture->pinch_threshold      = APP_GESTURE_PINCH_THRESHOLD;
    gesture->rotation_threshold   = APP_GESTURE_ROTATION_THRESHOLD_DEG;

    // Initialize history
    for (int i = 0; i < APP_GESTURE_HISTORY_MAX_POINTS; i++) {
        gesture->history_count[i] = 0;
        for (int j = 0; j < APP_GESTURE_HISTORY_MAX_LEN; j++) {
            gesture->history[i][j].x         = 0;
            gesture->history[i][j].y         = 0;
            gesture->history[i][j].timestamp = 0;
        }
    }

    // Initialize state
    gesture->current_gesture    = GESTURE_NONE;
    gesture->gesture_start_time = 0;
    gesture->gesture_start_x    = 0;
    gesture->gesture_start_y    = 0;

    // Initialize two-finger gesture data
    gesture->initial_distance     = 0;
    gesture->current_distance     = 0;
    gesture->initial_angle        = 0;
    gesture->current_angle        = 0;
    gesture->last_rotation_signed = 0;

    // Set callback
    gesture->gesture_callback = callback;
    gesture->user_data        = user_data;
}

gesture_type_t gesture_recognition_update(gesture_recognition_t *gesture, esp_lcd_touch_handle_t tp)
{
    // Read touch data
    esp_lcd_touch_read_data(tp);

    esp_lcd_touch_point_data_t touch_points[5];
    uint8_t touch_cnt = 0;
    esp_err_t ret     = esp_lcd_touch_get_data(tp, touch_points, &touch_cnt, 5);

    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000); // Get time in milliseconds

    if (ret != ESP_OK || touch_cnt == 0) {
        // No touch active - check for gesture completion

        // Check for swipe gesture (single finger)
        GESTURE_LOGD("No touch: checking swipe, history_count[0]=%d", gesture->history_count[0]);

        if (gesture->history_count[0] >= 2) {
            uint16_t start_x    = gesture->history[0][0].x;
            uint16_t start_y    = gesture->history[0][0].y;
            uint32_t start_time = gesture->history[0][0].timestamp;

            uint16_t end_x    = gesture->history[0][gesture->history_count[0] - 1].x;
            uint16_t end_y    = gesture->history[0][gesture->history_count[0] - 1].y;
            uint32_t end_time = gesture->history[0][gesture->history_count[0] - 1].timestamp;

            GESTURE_LOGD("Swipe check: start=(%d,%d), end=(%d,%d), duration=%lu", start_x, start_y, end_x, end_y,
                         end_time - start_time);

            float distance = calculate_distance(start_x, start_y, end_x, end_y);

            // Calculate velocity (pixels per millisecond)
            uint32_t duration = end_time - start_time;
            if (duration > 0) {
                float velocity = distance / duration;

                // Check if meets both distance and velocity thresholds
                // Thresholds calibrated based on actual user swipe data
                // Distance threshold: 50px (sufficient movement)
                // Velocity threshold: 0.01px/ms = 10px/s (ultra-low to support very slow swipes)
                if (distance >= gesture->swipe_threshold && velocity >= 0.01f) {
                    int dx = end_x - start_x;
                    int dy = end_y - start_y;

                    // Determine direction based on relative movement (best practice)
                    if (abs(dx) > abs(dy)) {
                        // Horizontal swipe
                        gesture->current_gesture = (dx > 0) ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
                    } else {
                        // Vertical swipe
                        gesture->current_gesture = (dy > 0) ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
                    }

                    ESP_LOGI(TAG, "Swipe detected: %s, distance: %.1f px, velocity: %.2f px/ms",
                             gesture->current_gesture == GESTURE_SWIPE_LEFT    ? "LEFT"
                             : gesture->current_gesture == GESTURE_SWIPE_RIGHT ? "RIGHT"
                             : gesture->current_gesture == GESTURE_SWIPE_UP    ? "UP"
                                                                               : "DOWN",
                             distance, velocity);

                    if (gesture->gesture_callback) {
                        gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
                    }

                    gesture_type_t detected_gesture = gesture->current_gesture;
                    gesture_recognition_reset(gesture);
                    return detected_gesture;
                }
            }
        }

        // Check for pinch gesture completion
        if (gesture->initial_distance > 0) {
            if (gesture->current_distance > 0) {
                float scale        = gesture->current_distance / gesture->initial_distance;
                float scale_change = fabs(scale - 1.0f);

                if (scale_change >= gesture->pinch_threshold) {
                    gesture->current_gesture = (scale > 1.0f) ? GESTURE_PINCH_OUT : GESTURE_PINCH_IN;

                    ESP_LOGI(TAG, "Pinch detected: %s, scale: %.2f",
                             gesture->current_gesture == GESTURE_PINCH_IN ? "IN" : "OUT", scale);

                    if (gesture->gesture_callback) {
                        gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
                    }

                    gesture_type_t detected_gesture = gesture->current_gesture;
                    gesture_recognition_reset(gesture);
                    return detected_gesture;
                }
            }
        }

        // Check for rotation gesture completion
        if (gesture->initial_angle != 0 && gesture->current_angle != 0) {
            float raw_delta    = gesture->current_angle - gesture->initial_angle;
            float angle_change = fabs(raw_delta);

            if (angle_change >= gesture->rotation_threshold) {
                gesture->current_gesture      = GESTURE_ROTATION;
                gesture->last_rotation_signed = raw_delta; // +CW / -CCW (degrees)

                ESP_LOGI(TAG, "Rotation detected: %.1f degrees (%s)", angle_change, raw_delta > 0 ? "CW" : "CCW");

                if (gesture->gesture_callback) {
                    gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
                }

                gesture_type_t detected_gesture = gesture->current_gesture;
                gesture_recognition_reset(gesture);
                return detected_gesture;
            }
        }

        // Reset when no touch active
        gesture_recognition_reset(gesture);
        return GESTURE_NONE;
    }

    // Touch is active - update tracking

    // Debug: Log touch point information
    GESTURE_LOGD("Touch active: count=%d, track_ids=[%d,%d,%d,%d,%d]", touch_cnt,
                 touch_cnt > 0 ? touch_points[0].track_id : -1, touch_cnt > 1 ? touch_points[1].track_id : -1,
                 touch_cnt > 2 ? touch_points[2].track_id : -1, touch_cnt > 3 ? touch_points[3].track_id : -1,
                 touch_cnt > 4 ? touch_points[4].track_id : -1);

    // Update touch point history (FIXED: use simple index instead of track_id)
    for (int i = 0; i < touch_cnt; i++) {
        // Use simple index 'i' instead of 'track_id' (to match data collector's approach)
        if (gesture->history_count[i] < 20) {
            gesture->history[i][gesture->history_count[i]].x         = touch_points[i].x;
            gesture->history[i][gesture->history_count[i]].y         = touch_points[i].y;
            gesture->history[i][gesture->history_count[i]].timestamp = current_time;
            gesture->history_count[i]++;

            GESTURE_LOGD("Point %d: pos=(%d,%d), history_count=%d", i, touch_points[i].x, touch_points[i].y,
                         gesture->history_count[i]);
        }
    }

    // Handle two-finger gestures first (priority over single touch to prevent misjudgment)
    if (touch_cnt >= 2) {
        uint16_t x1 = touch_points[0].x;
        uint16_t y1 = touch_points[0].y;
        uint16_t x2 = touch_points[1].x;
        uint16_t y2 = touch_points[1].y;

        // Calculate current distance and angle
        gesture->current_distance = calculate_distance(x1, y1, x2, y2);
        gesture->current_angle    = calculate_angle(x1, y1, x2, y2);

        // Initialize initial values if this is first two-finger touch (best practice)
        if (gesture->initial_distance == 0) {
            gesture->initial_distance = gesture->current_distance;
            gesture->initial_angle    = gesture->current_angle;
            ESP_LOGD(TAG, "Two-finger gesture initialized: distance=%.1f, angle=%.1f", gesture->initial_distance,
                     gesture->initial_angle);
        } else {
            // Real-time pinch detection (best practice)
            float scale        = gesture->current_distance / gesture->initial_distance;
            float scale_change = fabs(scale - 1.0f);

            if (scale_change >= gesture->pinch_threshold) {
                gesture->current_gesture = (scale > 1.0f) ? GESTURE_PINCH_OUT : GESTURE_PINCH_IN;

                ESP_LOGI(TAG, "Pinch detected: %s, scale: %.2f",
                         gesture->current_gesture == GESTURE_PINCH_IN ? "IN" : "OUT", scale);

                if (gesture->gesture_callback) {
                    gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
                }

                // Update initial distance to current for continuous pinch detection
                gesture->initial_distance = gesture->current_distance;

                return gesture->current_gesture;
            }

            // Real-time rotation detection (best practice)
            float raw_delta    = gesture->current_angle - gesture->initial_angle;
            float angle_change = fabs(raw_delta);

            if (angle_change >= gesture->rotation_threshold) {
                gesture->current_gesture      = GESTURE_ROTATION;
                gesture->last_rotation_signed = raw_delta; // +CW / -CCW (degrees)

                ESP_LOGI(TAG, "Rotation detected: %.1f degrees (%s)", angle_change, raw_delta > 0 ? "CW" : "CCW");

                if (gesture->gesture_callback) {
                    gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
                }

                // Update initial angle to current for continuous rotation detection
                gesture->initial_angle = gesture->current_angle;

                return gesture->current_gesture;
            }
        }

        // Cancel single-finger gesture tracking when two-finger gesture is active (prevent misjudgment)
        gesture->gesture_start_time = 0;
        gesture->history_count[0]   = 0;

        return GESTURE_NONE;
    }

    // Handle single touch gestures only when touch_cnt == 1 (strict check to prevent misjudgment)
    if (touch_cnt == 1) {
        // Initialize gesture start if this is first touch
        if (gesture->gesture_start_time == 0) {
            gesture->gesture_start_time = current_time;
            gesture->gesture_start_x    = touch_points[0].x;
            gesture->gesture_start_y    = touch_points[0].y;
            // REMOVED: history_count[0] = 0 - Keep history intact for swipe detection
        }

        // Check for long press
        uint32_t duration         = current_time - gesture->gesture_start_time;
        float distance_from_start = calculate_distance(gesture->gesture_start_x, gesture->gesture_start_y,
                                                       touch_points[0].x, touch_points[0].y);

        // Long press detected if duration met and position stable (best practice)
        if (duration >= gesture->long_press_threshold && distance_from_start < 30) {
            gesture->current_gesture = GESTURE_LONG_PRESS;
            ESP_LOGI(TAG, "Long press detected, duration: %lu ms", duration);

            if (gesture->gesture_callback) {
                gesture->gesture_callback(gesture->current_gesture, gesture->user_data);
            }

            return GESTURE_LONG_PRESS;
        }

        // REMOVED: Cancel long press logic (causes swipe detection to fail)
        // Long press and swipe are independent gestures - should not interfere
        // The swipe detection uses history which should remain intact
    }

    return GESTURE_NONE;
}

void gesture_recognition_get_details(gesture_recognition_t *gesture, float *scale, float *angle, uint16_t *distance,
                                     uint32_t *duration)
{
    if (gesture->initial_distance > 0 && gesture->current_distance > 0) {
        *scale = gesture->current_distance / gesture->initial_distance;
    } else {
        *scale = 1.0f;
    }

    if (gesture->initial_angle != 0 && gesture->current_angle != 0) {
        *angle = fabs(gesture->current_angle - gesture->initial_angle);
    } else {
        *angle = 0.0f;
    }

    if (gesture->history_count[0] >= 2) {
        *distance = (uint16_t)calculate_distance(gesture->history[0][0].x, gesture->history[0][0].y,
                                                 gesture->history[0][gesture->history_count[0] - 1].x,
                                                 gesture->history[0][gesture->history_count[0] - 1].y);
    } else {
        *distance = 0;
    }

    if (gesture->gesture_start_time > 0) {
        uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
        *duration             = current_time - gesture->gesture_start_time;
    } else {
        *duration = 0;
    }
}

void gesture_recognition_reset(gesture_recognition_t *gesture)
{
    // Reset history
    for (int i = 0; i < 5; i++) {
        gesture->history_count[i] = 0;
    }

    // Reset gesture state
    gesture->current_gesture    = GESTURE_NONE;
    gesture->gesture_start_time = 0;
    gesture->gesture_start_x    = 0;
    gesture->gesture_start_y    = 0;

    // Reset two-finger gesture data
    gesture->initial_distance = 0;
    gesture->current_distance = 0;
    gesture->initial_angle    = 0;
    gesture->current_angle    = 0;
    // Note: last_rotation_signed is intentionally NOT reset here so that
    // the callback can still read it after gesture_recognition_reset() is called.
}

// Gesture callback function for demo
static int gesture_count[9]                 = {0}; // Count for each gesture type
static gesture_type_t last_detected_gesture = GESTURE_NONE;

static void gesture_demo_callback(gesture_type_t gesture, void *user_data)
{
    if (gesture >= GESTURE_NONE && gesture <= GESTURE_LONG_PRESS) {
        gesture_count[gesture]++;
        last_detected_gesture = gesture;
    }
}

void gesture_recognition_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
    ESP_LOGI(TAG, "Starting Gesture Recognition Demo...");

    // Initialize gesture recognition
    gesture_recognition_t gesture_ctx;
    gesture_recognition_init(&gesture_ctx, gesture_demo_callback, NULL);

    // Clear screen
    gui_clear_screen(gui, COLOR_BLACK);

    // Draw title and instructions
    gui_draw_string(gui, 10, 10, "GESTURE RECOGNITION", COLOR_WHITE, COLOR_BLACK, 3);
    gui_draw_string(gui, 10, 50, "Try these gestures:", COLOR_YELLOW, COLOR_BLACK, 2);

    // Draw gesture instructions
    gui_draw_string(gui, 10, 90, "1. Swipe (Left/Right/Up/Down)", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 110, "2. Long press (>0.5 second)", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 130, "3. Pinch/Zoom (2 fingers)", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 150, "4. Rotation (2 fingers)", COLOR_CYAN, COLOR_BLACK, 1);

    // Draw gesture detection display area
    gui_draw_rect_outline(gui, 10, 180, 470, 420, COLOR_GREEN, 2);
    gui_draw_string(gui, 10, 185, "Gesture Detection Area", COLOR_GREEN, COLOR_BLACK, 1);

    // Draw gesture count display area
    gui_draw_string(gui, 10, 430, "Gesture Counts:", COLOR_WHITE, COLOR_BLACK, 2);

    // Gesture detection loop
    int frame_count                = 0;
    gesture_type_t current_gesture = GESTURE_NONE;

    while (frame_count < 600) { // Run for ~60 seconds
        // Update gesture recognition
        current_gesture = gesture_recognition_update(&gesture_ctx, tp);

        // Display detected gesture
        if (current_gesture != GESTURE_NONE) {
            // Clear previous gesture display
            gui_draw_filled_rect(gui, 20, 200, 450, 350, COLOR_BLACK);

            // Get gesture details
            float scale = 0, angle = 0;
            uint16_t distance = 0;
            uint32_t duration = 0;
            gesture_recognition_get_details(&gesture_ctx, &scale, &angle, &distance, &duration);

            // Display gesture type
            const char *gesture_name = "";
            uint32_t gesture_color   = COLOR_WHITE;

            switch (current_gesture) {
            case GESTURE_SWIPE_LEFT:
                gesture_name  = "SWIPE LEFT";
                gesture_color = COLOR_RED;
                break;
            case GESTURE_SWIPE_RIGHT:
                gesture_name  = "SWIPE RIGHT";
                gesture_color = COLOR_GREEN;
                break;
            case GESTURE_SWIPE_UP:
                gesture_name  = "SWIPE UP";
                gesture_color = COLOR_BLUE;
                break;
            case GESTURE_SWIPE_DOWN:
                gesture_name  = "SWIPE DOWN";
                gesture_color = COLOR_YELLOW;
                break;
            case GESTURE_PINCH_IN:
                gesture_name  = "PINCH IN";
                gesture_color = COLOR_MAGENTA;
                break;
            case GESTURE_PINCH_OUT:
                gesture_name  = "PINCH OUT";
                gesture_color = COLOR_CYAN;
                break;
            case GESTURE_ROTATION:
                gesture_name  = "ROTATION";
                gesture_color = COLOR_ORANGE;
                break;
            case GESTURE_LONG_PRESS:
                gesture_name  = "LONG PRESS";
                gesture_color = COLOR_GRAY;
                break;
            default:
                gesture_name  = "UNKNOWN";
                gesture_color = COLOR_WHITE;
                break;
            }

            // Draw gesture name in large font
            gui_draw_string(gui, 50, 220, gesture_name, gesture_color, COLOR_BLACK, 4);

            // Draw gesture details
            char details_str[64];

            if (current_gesture == GESTURE_SWIPE_LEFT || current_gesture == GESTURE_SWIPE_RIGHT ||
                current_gesture == GESTURE_SWIPE_UP || current_gesture == GESTURE_SWIPE_DOWN) {
                sprintf(details_str, "Distance: %d pixels", distance);
                gui_draw_string(gui, 50, 280, details_str, gesture_color, COLOR_BLACK, 2);
            }

            if (current_gesture == GESTURE_PINCH_IN || current_gesture == GESTURE_PINCH_OUT) {
                sprintf(details_str, "Scale: %.2f", scale);
                gui_draw_string(gui, 50, 280, details_str, gesture_color, COLOR_BLACK, 2);
            }

            if (current_gesture == GESTURE_ROTATION) {
                sprintf(details_str, "Angle: %.1f deg", angle);
                gui_draw_string(gui, 50, 280, details_str, gesture_color, COLOR_BLACK, 2);
            }

            if (current_gesture == GESTURE_LONG_PRESS) {
                sprintf(details_str, "Duration: %lu ms", duration);
                gui_draw_string(gui, 50, 280, details_str, gesture_color, COLOR_BLACK, 2);
            }
        }

        // Update gesture count display
        char count_str[32];
        const char *gesture_names[] = {"NONE", "SL", "SR", "SU", "SD", "PI", "PO", "ROT", "LP"};

        for (int i = 1; i <= 8; i++) { // Skip GESTURE_NONE (index 0)
            sprintf(count_str, "%s: %d", gesture_names[i], gesture_count[i]);
            gui_draw_string(gui, 10 + (i - 1) * 60, 460, count_str, COLOR_WHITE, COLOR_BLACK, 1);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50 FPS
        frame_count++;
    }

    // Display demo summary
    gui_clear_screen(gui, COLOR_BLACK);
    gui_draw_string(gui, 10, 10, "GESTURE DEMO COMPLETE", COLOR_WHITE, COLOR_BLACK, 4);

    int total_gestures = 0;
    for (int i = 1; i <= 8; i++) {
        total_gestures += gesture_count[i];
    }

    char summary_str[64];
    sprintf(summary_str, "Total gestures detected: %d", total_gestures);
    gui_draw_string(gui, 10, 60, summary_str, COLOR_GREEN, COLOR_BLACK, 3);

    ESP_LOGI(TAG, "Demo completed. Total gestures: %d", total_gestures);

    vTaskDelay(pdMS_TO_TICKS(3000));
}