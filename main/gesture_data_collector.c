/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gesture_data_collector.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "DATA_COLLECTOR";

// Helper function to calculate distance
static float calculate_distance(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

void gesture_data_collector_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
    ESP_LOGI(TAG, "=== GESTURE DATA COLLECTOR STARTED ===");
    ESP_LOGI(TAG, "Please follow screen instructions and perform gestures");
    ESP_LOGI(TAG, "Raw touch data will be logged for threshold calibration");
    
    // Clear screen
    gui_clear_screen(gui, COLOR_BLACK);
    
    // Display instructions
    gui_draw_string(gui, 10, 10, "GESTURE DATA COLLECTOR", COLOR_WHITE, COLOR_BLACK, 2);
    gui_draw_string(gui, 10, 40, "Purpose: Collect raw touch data", COLOR_YELLOW, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 60, "for gesture threshold calibration", COLOR_YELLOW, COLOR_BLACK, 1);
    
    gui_draw_string(gui, 10, 100, "Instructions:", COLOR_GREEN, COLOR_BLACK, 2);
    gui_draw_string(gui, 10, 120, "1. Perform a SWIPE gesture", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 140, "2. Try different speeds", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 160, "3. Observe logged data", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 180, "4. Copy logs to tmp_log.txt", COLOR_CYAN, COLOR_BLACK, 1);
    
    // Draw test area
    gui_draw_rect_outline(gui, 50, 220, 430, 400, COLOR_MAGENTA, 2);
    gui_draw_string(gui, 50, 225, "TEST AREA", COLOR_MAGENTA, COLOR_BLACK, 1);
    
    // Data collection state
    uint16_t start_x = 0, start_y = 0;
    uint16_t last_x = 0, last_y = 0;  // Track last position during swipe
    uint32_t start_time = 0;
    uint32_t last_time = 0;  // Track last time during swipe
    uint32_t last_log_time = 0;  // Track last log output time
    bool tracking = false;
    int gesture_count = 0;
    const int max_gestures = 5; // Collect 5 swipe samples
    
    ESP_LOGI(TAG, "Waiting for swipe gestures (%d samples)...", max_gestures);
    
    while (gesture_count < max_gestures) {
        // Read touch data
        esp_lcd_touch_read_data(tp);
        
        esp_lcd_touch_point_data_t touch_points[5];
        uint8_t touch_cnt = 0;
        esp_err_t ret = esp_lcd_touch_get_data(tp, touch_points, &touch_cnt, 5);
        
        uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000); // milliseconds
        
        if (ret == ESP_OK && touch_cnt > 0) {
            if (!tracking && touch_cnt == 1) {
                // Start tracking
                tracking = true;
                start_x = touch_points[0].x;
                start_y = touch_points[0].y;
                start_time = current_time;
                last_x = start_x;
                last_y = start_y;
                last_time = start_time;
                
                // Visual feedback
                gui_draw_filled_circle(gui, start_x, start_y, 10, COLOR_GREEN);
                gui_draw_string(gui, 50, 250, "TRACKING START", COLOR_GREEN, COLOR_BLACK, 2);
                
                ESP_LOGI(TAG, "=== SWIPE #%d START ===", gesture_count + 1);
                ESP_LOGI(TAG, "  Start position: (%d, %d)", start_x, start_y);
                ESP_LOGI(TAG, "  Start time: %lu ms", start_time);
            }
            
            if (tracking && touch_cnt == 1) {
                // Update tracking - save last position and time
                uint16_t current_x = touch_points[0].x;
                uint16_t current_y = touch_points[0].y;
                
                last_x = current_x;  // Save last position
                last_y = current_y;
                last_time = current_time;  // Save last time
                
                // Draw current position
                gui_draw_filled_circle(gui, current_x, current_y, 5, COLOR_CYAN);
                
                // Calculate current distance
                float current_distance = calculate_distance(start_x, start_y, current_x, current_y);
                uint32_t current_duration = current_time - start_time;
                
                // Display real-time data on screen (clear and redraw to prevent overlap)
                gui_draw_filled_rect(gui, 60, 280, 420, 340, COLOR_BLACK);
                
                char status_str[64];
                sprintf(status_str, "Distance: %.1f px", current_distance);
                gui_draw_string(gui, 60, 280, status_str, COLOR_WHITE, COLOR_BLACK, 1);
                
                sprintf(status_str, "Duration: %lu ms", current_duration);
                gui_draw_string(gui, 60, 300, status_str, COLOR_WHITE, COLOR_BLACK, 1);
                
                if (current_duration > 0) {
                    float velocity = current_distance / current_duration;
                    sprintf(status_str, "Velocity: %.3f px/ms", velocity);
                    gui_draw_string(gui, 60, 320, status_str, COLOR_WHITE, COLOR_BLACK, 1);
                }
                
                // Log data every 200ms (fixed timing check)
                if (current_time - last_log_time >= 200) {
                    float velocity = current_duration > 0 ? current_distance / current_duration : 0;
                    ESP_LOGI(TAG, "  Time: %lu ms, Pos: (%d,%d), Dist: %.1f px, Vel: %.3f px/ms",
                             current_duration, current_x, current_y, current_distance, velocity);
                    last_log_time = current_time;
                }
            }
        }
        
        if (tracking && touch_cnt == 0) {
            // Swipe ended - use saved last position and time
            tracking = false;
            gesture_count++;
            
            // Use saved last position and time (FIXED)
            uint16_t end_x = last_x;
            uint16_t end_y = last_y;
            uint32_t end_time = last_time;
            
            // Calculate final results (FIXED)
            float final_distance = calculate_distance(start_x, start_y, end_x, end_y);
            uint32_t final_duration = end_time - start_time;
            float final_velocity = final_duration > 0 ? final_distance / final_duration : 0;
            
            ESP_LOGI(TAG, "=== SWIPE #%d END ===", gesture_count);
            ESP_LOGI(TAG, "  End position: (%d, %d)", end_x, end_y);
            ESP_LOGI(TAG, "  End time: %lu ms", end_time);
            ESP_LOGI(TAG, "  Duration: %lu ms", final_duration);
            ESP_LOGI(TAG, "  Distance: %.1f px", final_distance);
            ESP_LOGI(TAG, "  Velocity: %.3f px/ms (%.1f px/s)", final_velocity, final_velocity * 1000.0f);
            
            // Determine direction
            int dx = end_x - start_x;
            int dy = end_y - start_y;
            
            if (abs(dx) > abs(dy)) {
                ESP_LOGI(TAG, "  Direction: %s (dx=%d)", dx > 0 ? "RIGHT" : "LEFT", dx);
            } else {
                ESP_LOGI(TAG, "  Direction: %s (dy=%d)", dy > 0 ? "DOWN" : "UP", dy);
            }
            
            // Threshold analysis
            ESP_LOGI(TAG, "  === Threshold Analysis ===");
            ESP_LOGI(TAG, "  Distance threshold: 50 px (result: %s)", 
                     final_distance >= 50.0f ? "PASS" : "FAIL");
            ESP_LOGI(TAG, "  Velocity threshold: 0.05 px/ms (result: %s)",
                     final_velocity >= 0.05f ? "PASS" : "FAIL");
            ESP_LOGI(TAG, "  Combined result: %s", 
                     (final_distance >= 50.0f && final_velocity >= 0.05f) ? "GESTURE DETECTED" : "GESTURE NOT DETECTED");
            
            // Display result on screen
            gui_draw_filled_rect(gui, 50, 280, 430, 380, COLOR_BLACK);
            
            char result_str[64];
            sprintf(result_str, "Swipe #%d Complete", gesture_count);
            gui_draw_string(gui, 60, 280, result_str, COLOR_GREEN, COLOR_BLACK, 2);
            
            sprintf(result_str, "Dist: %.1f px | Vel: %.3f", final_distance, final_velocity);
            gui_draw_string(gui, 60, 310, result_str, COLOR_YELLOW, COLOR_BLACK, 1);
            
            sprintf(result_str, "Result: %s", 
                     (final_distance >= 50.0f && final_velocity >= 0.05f) ? "DETECTED" : "NOT DETECTED");
            gui_draw_string(gui, 60, 340, result_str, 
                            (final_distance >= 50.0f && final_velocity >= 0.05f) ? COLOR_GREEN : COLOR_RED, 
                            COLOR_BLACK, 2);
            
            // Wait 3 seconds before next sample
            if (gesture_count < max_gestures) {
                gui_draw_string(gui, 60, 380, "Next sample in 3 sec...", COLOR_CYAN, COLOR_BLACK, 1);
                vTaskDelay(pdMS_TO_TICKS(3000));
                
                // Clear test area
                gui_draw_filled_rect(gui, 50, 250, 430, 400, COLOR_BLACK);
                gui_draw_rect_outline(gui, 50, 220, 430, 400, COLOR_MAGENTA, 2);
            }
            
            // Reset last_log_time for next gesture
            last_log_time = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(20)); // 50 FPS sampling
    }
    
    // Display summary
    gui_clear_screen(gui, COLOR_BLACK);
    gui_draw_string(gui, 10, 10, "DATA COLLECTION COMPLETE", COLOR_WHITE, COLOR_BLACK, 3);
    gui_draw_string(gui, 10, 50, "Please copy logs to tmp_log.txt", COLOR_YELLOW, COLOR_BLACK, 2);
    
    ESP_LOGI(TAG, "=== DATA COLLECTION COMPLETE ===");
    ESP_LOGI(TAG, "Collected %d swipe samples", gesture_count);
    ESP_LOGI(TAG, "Please review logs and adjust thresholds accordingly");
    
    vTaskDelay(pdMS_TO_TICKS(5000));
}