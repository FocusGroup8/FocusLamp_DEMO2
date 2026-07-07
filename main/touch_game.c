/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_game.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include "board_config.h"

static const char *TAG = "TOUCH_GAME";

// Ball state
typedef struct {
    int x;              // Ball position x
    int y;              // Ball position y
    int radius;         // Ball radius (adjustable via pinch)
    float rotation;     // Ball rotation angle (adjustable via rotation gesture)
    int velocity_x;     // Ball velocity x (controlled by swipe)
    int velocity_y;     // Ball velocity y (controlled by swipe)
    uint32_t color;     // Ball color
    bool paused;        // Game paused state
    int score;          // Game score
} ball_state_t;

// Draw rotating ball with visual effect
static void draw_ball(simple_gui_t *gui, ball_state_t *ball)
{
    // Draw ball shadow
    gui_draw_filled_circle(gui, ball->x + 3, ball->y + 3, ball->radius, COLOR_GRAY);
    
    // Draw main ball
    gui_draw_filled_circle(gui, ball->x, ball->y, ball->radius, ball->color);
    
    // Draw rotation indicator (line inside ball)
    int indicator_length = ball->radius - 5;
    int end_x = ball->x + (int)(indicator_length * cos(ball->rotation));
    int end_y = ball->y + (int)(indicator_length * sin(ball->rotation));
    gui_draw_line(gui, ball->x, ball->y, end_x, end_y, COLOR_WHITE);
    
    // Draw ball outline (circle)
    gui_draw_rect_outline(gui, ball->x - ball->radius, ball->y - ball->radius,
                         ball->x + ball->radius, ball->y + ball->radius, COLOR_WHITE, 1);
}

// Gesture callback function for game control
static ball_state_t *game_ball = NULL;

static void game_gesture_callback(gesture_type_t gesture, void *user_data)
{
    if (!game_ball) return;
    
    ESP_LOGI(TAG, "Gesture detected in game: %d", gesture);
    
    switch (gesture) {
        case GESTURE_SWIPE_LEFT:
            game_ball->velocity_x = -APP_GAME_BALL_VELOCITY;
            game_ball->velocity_y = 0;
            game_ball->color = COLOR_RED;
            break;

        case GESTURE_SWIPE_RIGHT:
            game_ball->velocity_x = APP_GAME_BALL_VELOCITY;
            game_ball->velocity_y = 0;
            game_ball->color = COLOR_GREEN;
            break;

        case GESTURE_SWIPE_UP:
            game_ball->velocity_x = 0;
            game_ball->velocity_y = -APP_GAME_BALL_VELOCITY;
            game_ball->color = COLOR_BLUE;
            break;

        case GESTURE_SWIPE_DOWN:
            game_ball->velocity_x = 0;
            game_ball->velocity_y = APP_GAME_BALL_VELOCITY;
            game_ball->color = COLOR_YELLOW;
            break;

        case GESTURE_PINCH_IN:
            if (game_ball->radius > APP_GAME_BALL_MIN_RADIUS) {
                game_ball->radius -= APP_GAME_BALL_RADIUS_STEP;
                game_ball->score += APP_GAME_SCORE_PINCH;
                ESP_LOGI(TAG, "Ball size decreased: radius=%d, score=%d", game_ball->radius, game_ball->score);
            }
            break;

        case GESTURE_PINCH_OUT:
            if (game_ball->radius < APP_GAME_BALL_MAX_RADIUS) {
                game_ball->radius += APP_GAME_BALL_RADIUS_STEP;
                game_ball->score += APP_GAME_SCORE_PINCH;
                ESP_LOGI(TAG, "Ball size increased: radius=%d, score=%d", game_ball->radius, game_ball->score);
            }
            break;

        case GESTURE_ROTATION:
            game_ball->rotation += APP_GAME_BALL_ROTATION_STEP_RAD;
            game_ball->score += APP_GAME_SCORE_ROTATION;
            ESP_LOGI(TAG, "Ball rotated: angle=%.1f, score=%d", game_ball->rotation * 180 / M_PI, game_ball->score);
            break;
            
        case GESTURE_LONG_PRESS:
            game_ball->paused = !game_ball->paused;
            ESP_LOGI(TAG, "Game %s", game_ball->paused ? "PAUSED" : "RESUMED");
            break;
            
        default:
            break;
    }
}

void touch_game_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
    ESP_LOGI(TAG, "Starting Touch Game Demo...");
    
    // Initialize ball state
    ball_state_t ball = {
        .x = APP_GAME_BALL_INIT_X,
        .y = APP_GAME_BALL_INIT_Y,
        .radius = APP_GAME_BALL_INIT_RADIUS,
        .rotation = 0,
        .velocity_x = 0,
        .velocity_y = 0,
        .color = APP_GAME_BALL_INIT_COLOR,
        .paused = false,
        .score = 0
    };
    game_ball = &ball;
    
    // Initialize gesture recognition
    gesture_recognition_t gesture_ctx;
    gesture_recognition_init(&gesture_ctx, game_gesture_callback, NULL);
    
    // Clear screen
    gui_clear_screen(gui, COLOR_BLACK);
    
    // Draw game title and instructions
    gui_draw_string(gui, 10, 10, "GESTURE BALL GAME", COLOR_WHITE, COLOR_BLACK, 3);
    
    // Draw game instructions
    gui_draw_string(gui, 10, 50, "Controls:", COLOR_YELLOW, COLOR_BLACK, 2);
    gui_draw_string(gui, 10, 80, "Swipe: Move ball", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 100, "Pinch: Change size", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 120, "Rotate: Rotate ball", COLOR_CYAN, COLOR_BLACK, 1);
    gui_draw_string(gui, 10, 140, "Long press: Pause", COLOR_CYAN, COLOR_BLACK, 1);
    
    // Draw game area
    gui_draw_rect_outline(gui, APP_GAME_AREA_OUTLINE_X1, APP_GAME_AREA_OUTLINE_Y1,
                          APP_GAME_AREA_OUTLINE_X2, APP_GAME_AREA_OUTLINE_Y2, COLOR_GREEN, 2);
    gui_draw_string(gui, 10, 175, "Game Area", COLOR_GREEN, COLOR_BLACK, 1);

    // Game loop
    int frame_count = 0;

    while (frame_count < APP_GAME_MAX_FRAMES) {
        
        // Update gesture recognition
        gesture_recognition_update(&gesture_ctx, tp);
        
        // Update ball position (if not paused)
        if (!ball.paused) {
            ball.x += ball.velocity_x;
            ball.y += ball.velocity_y;
            
            // Boundary checking (keep ball in game area)
            if (ball.x < APP_GAME_AREA_X_MIN) {
                ball.x = APP_GAME_AREA_X_MIN;
                ball.velocity_x = 0;
                ball.score += APP_GAME_SCORE_BOUNDARY;
                ESP_LOGI(TAG, "Boundary hit LEFT: score=%d", ball.score);
            }
            if (ball.x > APP_GAME_AREA_X_MAX) {
                ball.x = APP_GAME_AREA_X_MAX;
                ball.velocity_x = 0;
                ball.score += APP_GAME_SCORE_BOUNDARY;
                ESP_LOGI(TAG, "Boundary hit RIGHT: score=%d", ball.score);
            }
            if (ball.y < APP_GAME_AREA_Y_MIN) {
                ball.y = APP_GAME_AREA_Y_MIN;
                ball.velocity_y = 0;
                ball.score += APP_GAME_SCORE_BOUNDARY;
                ESP_LOGI(TAG, "Boundary hit TOP: score=%d", ball.score);
            }
            if (ball.y > APP_GAME_AREA_Y_MAX) {
                ball.y = APP_GAME_AREA_Y_MAX;
                ball.velocity_y = 0;
                ball.score += APP_GAME_SCORE_BOUNDARY;
                ESP_LOGI(TAG, "Boundary hit BOTTOM: score=%d", ball.score);
            }

            // Update rotation animation (continuous rotation effect)
            ball.rotation += APP_GAME_BALL_ROTATION_SPEED;
            if (ball.rotation >= 2 * M_PI) {
                ball.rotation -= 2 * M_PI;
            }
        }
        
        // Clear game area
        gui_draw_filled_rect(gui, 15, 185, 465, 395, COLOR_BLACK);
        
        // Draw ball
        draw_ball(gui, &ball);
        
        // Draw score
        char score_str[32];
        sprintf(score_str, "Score: %d", ball.score);
        gui_draw_string(gui, 10, 410, score_str, COLOR_WHITE, COLOR_BLACK, 2);
        
        // Draw pause indicator
        if (ball.paused) {
            gui_draw_string(gui, 10, 440, "PAUSED", COLOR_RED, COLOR_BLACK, 3);
        }
        
        // Draw velocity indicator
        if (ball.velocity_x != 0 || ball.velocity_y != 0) {
            char vel_str[32];
            sprintf(vel_str, "Velocity: (%d, %d)", ball.velocity_x, ball.velocity_y);
            gui_draw_string(gui, 10, 460, vel_str, ball.color, COLOR_BLACK, 1);
        }
        
        // Frame timing
        vTaskDelay(pdMS_TO_TICKS(APP_GAME_FRAME_DELAY_MS));
        frame_count++;
    }
    
    // Game complete message
    gui_clear_screen(gui, COLOR_BLACK);
    gui_draw_string(gui, 10, 10, "GAME COMPLETE!", COLOR_WHITE, COLOR_BLACK, 4);
    
    char final_score[32];
    sprintf(final_score, "Final Score: %d", ball.score);
    gui_draw_string(gui, 10, 80, final_score, COLOR_YELLOW, COLOR_BLACK, 3);
    
    gui_draw_string(gui, 10, 150, "Thank you for playing!", COLOR_CYAN, COLOR_BLACK, 2);
    
    ESP_LOGI(TAG, "Game demo completed. Final score: %d", ball.score);
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    game_ball = NULL;
}