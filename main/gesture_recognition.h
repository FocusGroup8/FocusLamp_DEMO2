/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_touch.h"
#include "simple_gui.h"
#include <stdint.h>

/**
 * @brief Gesture recognition library for multi-touch interactions
 * 
 * This library provides gesture detection capabilities including:
 * - Swipe (left/right/up/down)
 * - Pinch/Zoom (two-finger scaling)
 * - Rotation (two-finger rotation)
 * - Long press detection
 */

// Gesture types
typedef enum {
    GESTURE_NONE,           // No gesture detected
    GESTURE_SWIPE_LEFT,     // Swipe to the left
    GESTURE_SWIPE_RIGHT,    // Swipe to the right
    GESTURE_SWIPE_UP,       // Swipe upward
    GESTURE_SWIPE_DOWN,     // Swipe downward
    GESTURE_PINCH_IN,       // Two-finger pinch (zoom out)
    GESTURE_PINCH_OUT,      // Two-finger spread (zoom in)
    GESTURE_ROTATION,       // Two-finger rotation
    GESTURE_LONG_PRESS,     // Long press detected
} gesture_type_t;

// Gesture direction (for swipe gestures)
typedef enum {
    GESTURE_DIR_NONE,
    GESTURE_DIR_HORIZONTAL,
    GESTURE_DIR_VERTICAL,
} gesture_direction_t;

/**
 * @brief Touch point history structure for gesture tracking
 */
typedef struct {
    uint16_t x;             // X coordinate
    uint16_t y;             // Y coordinate
    uint32_t timestamp;     // Timestamp in milliseconds
} touch_point_history_t;

/**
 * @brief Gesture recognition context structure
 */
typedef struct {
    // Gesture detection thresholds
    uint16_t swipe_threshold;       // Minimum distance for swipe detection (pixels)
    uint16_t long_press_threshold;  // Minimum duration for long press (milliseconds)
    float pinch_threshold;          // Minimum scale change for pinch detection
    float rotation_threshold;       // Minimum angle change for rotation detection (degrees)
    
    // Touch point history (for tracking gesture trajectory)
    touch_point_history_t history[5][20]; // 5 touch points, 20 history records each
    uint8_t history_count[5];                // History count for each touch point
    
    // Current gesture detection state
    gesture_type_t current_gesture;          // Currently detected gesture
    uint32_t gesture_start_time;             // Gesture start timestamp
    uint16_t gesture_start_x;                // Gesture start X coordinate
    uint16_t gesture_start_y;                // Gesture start Y coordinate
    
    // Two-finger gesture data
    float initial_distance;                  // Initial distance between two fingers
    float current_distance;                  // Current distance between two fingers
    float initial_angle;                     // Initial angle between two fingers
    float current_angle;                     // Current angle between two fingers
    
    // Gesture callback
    void (*gesture_callback)(gesture_type_t gesture, void *user_data);
    void *user_data;
} gesture_recognition_t;

/**
 * @brief Initialize gesture recognition context
 * 
 * @param gesture Pointer to gesture recognition structure
 * @param callback Gesture detection callback function (optional)
 * @param user_data User data for callback (optional)
 */
void gesture_recognition_init(gesture_recognition_t *gesture,
                              void (*callback)(gesture_type_t, void*),
                              void *user_data);

/**
 * @brief Update gesture recognition with new touch data
 * 
 * @param gesture Pointer to gesture recognition structure
 * @param tp Pointer to touch handle
 * @return Detected gesture type
 */
gesture_type_t gesture_recognition_update(gesture_recognition_t *gesture,
                                           esp_lcd_touch_handle_t tp);

/**
 * @brief Get gesture details (for advanced gesture types)
 * 
 * @param gesture Pointer to gesture recognition structure
 * @param scale Output: scale factor for pinch gesture
 * @param angle Output: rotation angle for rotation gesture
 * @param distance Output: swipe distance
 * @param duration Output: gesture duration
 */
void gesture_recognition_get_details(gesture_recognition_t *gesture,
                                     float *scale, float *angle,
                                     uint16_t *distance, uint32_t *duration);

/**
 * @brief Clear gesture history (reset tracking)
 * 
 * @param gesture Pointer to gesture recognition structure
 */
void gesture_recognition_reset(gesture_recognition_t *gesture);

/**
 * @brief Demo function to showcase gesture recognition capabilities
 * 
 * @param gui Pointer to GUI context
 * @param tp Pointer to touch handle
 */
void gesture_recognition_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp);