#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wake_word_engine_config.h"
#include "wake_word_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize wake word engine
 *
 * Creates AFE + MultiNet pipeline for continuous wake word detection.
 * Must be called before audio_bridge_mic_start() so the PCM callback
 * is registered in time.
 *
 * @param config Configuration for wake word engine
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_init(const wake_word_engine_config_t *config);

/**
 * @brief Deinitialize wake word engine
 *
 * Stops detection, destroys AFE and MultiNet models, frees resources.
 */
esp_err_t wake_word_engine_deinit(void);

/**
 * @brief Start continuous wake word detection
 *
 * Begins processing PCM audio from audio_bridge for wake word detection.
 * The engine will call the detect_cb when a wake word is recognized.
 *
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_start(void);

/**
 * @brief Stop wake word detection
 *
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_stop(void);

/**
 * @brief Pause wake word detection (e.g., during TTS playback)
 *
 * Keeps AFE and MultiNet loaded but skips detection processing.
 * Used to prevent self-triggering from TTS audio output.
 *
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_pause(void);

/**
 * @brief Resume wake word detection after pause
 *
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_resume(void);

/**
 * @brief Mark TTS playback active/inactive (enables barge-in VAD)
 *
 * While active, the AFE keeps running with AEC so the engine can detect
 * user speech (barge-in) on the echo-cancelled signal. MultiNet wake word
 * detection stays disabled during this window. Call with true on TTS_START
 * and false on TTS_STOP.
 *
 * @param active  true while TTS is playing
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_set_tts_active(bool active);

/**
 * @brief Switch language model at runtime
 *
 * Destroys current MultiNet model and creates one for the new language.
 * Detection must be stopped before switching.
 *
 * @param lang Target language (CN or EN)
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_set_language(wake_word_lang_t lang);

/**
 * @brief Get current language
 *
 * @return Current language setting
 */
wake_word_lang_t wake_word_engine_get_language(void);

/**
 * @brief Add a custom wake word command
 *
 * Adds a command phrase to the current language's command list.
 * For Chinese, use pinyin format (e.g., "ni hao xiao zhi").
 * For English, use normal spelling (e.g., "hey jarvis").
 * Must call wake_word_engine_update_commands() after adding all commands.
 *
 * @param command_id Unique command ID (user-defined, > 0)
 * @param phrase Command phrase string (pinyin for CN, spelling for EN)
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_add_command(int command_id, const char *phrase);

/**
 * @brief Remove a wake word command by phrase
 *
 * @param phrase The command phrase to remove
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_remove_command(const char *phrase);

/**
 * @brief Clear all wake word commands
 *
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_clear_commands(void);

/**
 * @brief Update commands after add/remove/clear
 *
 * Must be called after wake_word_engine_add_command/remove_command/clear_commands
 * to rebuild the language model. Returns error phrases that could not be parsed.
 *
 * @return NULL on success, or pointer to error list (caller must free with wake_word_engine_free_error)
 */
void *wake_word_engine_update_commands(void);

/**
 * @brief Free error list returned by wake_word_engine_update_commands()
 *
 * @param err  Error pointer returned by update_commands, or NULL
 */
void wake_word_engine_free_error(void *err);

/**
 * @brief Get current engine state
 *
 * @return Current state
 */
wake_word_state_t wake_word_engine_get_state(void);

/**
 * @brief Set detection threshold
 *
 * @param threshold Detection threshold (0.0 ~ 0.9999)
 * @return ESP_OK on success
 */
esp_err_t wake_word_engine_set_threshold(float threshold);

/**
 * @brief Feed PCM audio data to the engine
 *
 * Called from audio_bridge PCM callback. Processes audio through AFE
 * and runs MultiNet detection.
 *
 * @param pcm_data 16-bit signed PCM samples (16kHz mono)
 * @param sample_count Number of samples
 */
void wake_word_engine_feed_pcm(const int16_t *pcm_data, int sample_count);

#if (WAKE_WORD_ENGINE_ENABLE == 0)

/* Disabled stubs */
static inline esp_err_t wake_word_engine_init(const wake_word_engine_config_t *config) { (void)config; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_pause(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_resume(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_set_tts_active(bool active) { (void)active; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_set_language(wake_word_lang_t lang) { (void)lang; return ESP_ERR_NOT_SUPPORTED; }
static inline wake_word_lang_t wake_word_engine_get_language(void) { return WAKE_WORD_LANG_CN; }
static inline esp_err_t wake_word_engine_add_command(int command_id, const char *phrase) { (void)command_id; (void)phrase; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_remove_command(const char *phrase) { (void)phrase; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wake_word_engine_clear_commands(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void *wake_word_engine_update_commands(void) { return NULL; }
static inline void wake_word_engine_free_error(void *err) { (void)err; }
static inline wake_word_state_t wake_word_engine_get_state(void) { return WAKE_WORD_STATE_IDLE; }
static inline esp_err_t wake_word_engine_set_threshold(float threshold) { (void)threshold; return ESP_ERR_NOT_SUPPORTED; }
static inline void wake_word_engine_feed_pcm(const int16_t *pcm_data, int sample_count) { (void)pcm_data; (void)sample_count; }

#endif /* WAKE_WORD_ENGINE_ENABLE == 0 */

#ifdef __cplusplus
}
#endif
