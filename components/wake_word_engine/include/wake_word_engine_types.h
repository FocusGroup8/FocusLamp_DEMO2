#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wake word engine language selection
 */
typedef enum {
    WAKE_WORD_LANG_CN = 0,  /*!< Chinese (mn7_cn) */
    WAKE_WORD_LANG_EN = 1,  /*!< English (mn7_en) */
} wake_word_lang_t;

/**
 * @brief Wake word engine state
 */
typedef enum {
    WAKE_WORD_STATE_IDLE = 0,       /*!< Not initialized or stopped */
    WAKE_WORD_STATE_LISTENING = 1,  /*!< Actively listening for wake word */
    WAKE_WORD_STATE_DETECTED = 2,   /*!< Wake word detected, command phrase being recognized */
    WAKE_WORD_STATE_PAUSED = 3,     /*!< Temporarily paused (e.g., during TTS playback) */
} wake_word_state_t;

/**
 * @brief Wake word detection event callback
 *
 * Called when a wake word / command phrase is detected.
 *
 * @param command_id  The command ID that was registered with the wake word
 * @param command_str The command string that was recognized
 * @param lang        The language model that detected the command
 * @param prob        Detection probability (0.0 ~ 1.0)
 * @param ctx         User context
 */
typedef void (*wake_word_detect_cb_t)(int command_id, const char *command_str,
                                      wake_word_lang_t lang, float prob, void *ctx);

/**
 * @brief Wake word engine configuration
 */
typedef struct {
    wake_word_lang_t default_lang;       /*!< Default language (CN or EN) */
    wake_word_detect_cb_t detect_cb;     /*!< Detection result callback */
    void *detect_cb_ctx;                 /*!< User context for callback */
    float det_threshold;                 /*!< Detection threshold (0.0 ~ 0.9999), 0 = use default */
    int det_timeout_ms;                  /*!< Detection timeout in ms for command phrase */
} wake_word_engine_config_t;

#define WAKE_WORD_ENGINE_DEFAULT_CONFIG() { \
    .default_lang = WAKE_WORD_LANG_CN,     \
    .detect_cb = NULL,                     \
    .detect_cb_ctx = NULL,                 \
    .det_threshold = 0.0,                  \
    .det_timeout_ms = 2000,                \
}

#ifdef __cplusplus
}
#endif
