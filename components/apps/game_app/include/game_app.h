/*
 * game_app.h - Mini-game application module for FocusLamp
 */

#pragma once
#ifndef __GAME_APP_H__
#define __GAME_APP_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Game ID Enumeration ===================== */
typedef enum {
    GAME_ID_ROCK_PAPER_SCISSORS = 0,   /* 猜拳游戏 */
    GAME_ID_REACTION_TEST,              /* 反应测试 */
    GAME_ID_RHYTHM_GAME,                /* 节奏游戏 */
    GAME_ID_MEMORY_GAME,                /* 记忆游戏 */
    GAME_ID_MAX,
} game_id_t;

/**
 * @brief Initialize game application.
 *        Subscribes to EV_APP_MODE_CHANGED, touch events, and EV_APP_GAME_EVENT.
 * @return esp_err_t
 */
esp_err_t game_app_init(void);

/**
 * @brief Start a specific game.
 * @param game_id  Game identifier to launch
 * @return esp_err_t
 */
esp_err_t game_app_start(game_id_t game_id);

/**
 * @brief Stop the currently running game.
 * @return esp_err_t
 */
esp_err_t game_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_APP_H__ */