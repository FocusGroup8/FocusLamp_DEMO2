#ifndef SMART_VOICE_CONTROL_MODULE_H
#define SMART_VOICE_CONTROL_MODULE_H

#include "esp_err.h"

#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

    /**
     * @brief Initialize Smart Voice Control Module
     *
     * This function initializes the module with the given configuration.
     * It will:
     * - Load voice command configuration from JSON file (or use defaults)
     * - Initialize audio processor (AEC/VAD/WakeWord)
     * - Initialize network monitor
     * - Prepare cloud integration
     *
     * @param config Configuration parameters (NULL to use Kconfig defaults)
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_ARG: Invalid argument
     *         - ESP_ERR_NO_MEM: Memory allocation failed
     *         - ESP_ERR_NOT_FOUND: Config file not found (will use defaults)
     */
    esp_err_t smart_voice_control_module_init(const smart_voice_config_params_t* config);

    /**
     * @brief Deinitialize Smart Voice Control Module
     *
     * Release all resources and stop all tasks.
     *
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_STATE: Not initialized
     */
    esp_err_t smart_voice_control_module_deinit(void);

    /**
     * @brief Start Smart Voice Control Module
     *
     * Start the main task and begin listening for wake word.
     *
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_STATE: Not initialized or already running
     */
    esp_err_t smart_voice_control_module_start(void);

    /**
     * @brief Stop Smart Voice Control Module
     *
     * Stop the main task and release audio resources.
     *
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_STATE: Not running
     */
    esp_err_t smart_voice_control_module_stop(void);

    /**
     * @brief Set callback functions
     *
     * Register callbacks for various events.
     *
     * @param callbacks Callback structure (can be NULL to clear callbacks)
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_ARG: Invalid argument
     */
    esp_err_t smart_voice_control_module_set_callbacks(const smart_voice_callbacks_t* callbacks);

    /**
     * @brief Get current operation mode
     *
     * @return smart_voice_mode_t Current mode
     */
    smart_voice_mode_t smart_voice_control_module_get_mode(void);

    /**
     * @brief Set operation mode
     *
     * Force switch to a specific mode.
     * Note: In auto-fallback mode, network events may override this setting.
     *
     * @param mode Target mode
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_ARG: Invalid mode
     */
    esp_err_t smart_voice_control_module_set_mode(smart_voice_mode_t mode);

    /**
     * @brief Get current state
     *
     * @return smart_voice_state_t Current state
     */
    smart_voice_state_t smart_voice_control_module_get_state(void);

    /**
     * @brief Get current network status
     *
     * @return smart_voice_network_status_t Network status
     */
    smart_voice_network_status_t smart_voice_control_module_get_network_status(void);

    /**
     * @brief Check if wake word is detected
     *
     * @return true if wake word detected recently
     */
    bool smart_voice_control_module_is_wake_word_detected(void);

    /**
     * @brief Get loaded command count
     *
     * @return int Number of loaded commands
     */
    int smart_voice_control_module_get_command_count(void);

    /**
     * @brief Get command by index
     *
     * @param index Command index (0-based)
     * @return const smart_voice_command_t* Pointer to command or NULL if invalid
     */
    const smart_voice_command_t* smart_voice_control_module_get_command(int index);

    /**
     * @brief Find command by keyword
     *
     * @param keyword Search keyword
     * @return const smart_voice_command_t* Pointer to command or NULL if not found
     */
    const smart_voice_command_t* smart_voice_control_module_find_command(const char* keyword);

    /**
     * @brief Reload configuration from file
     *
     * Reload voice commands from JSON file.
     * Requires CONFIG_SMART_VOICE_ENABLE_HOT_RELOAD enabled.
     *
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_NOT_SUPPORTED: Hot reload not enabled
     *         - ESP_ERR_NOT_FOUND: File not found
     *         - ESP_ERR_INVALID_FORMAT: JSON parse error
     */
    esp_err_t smart_voice_control_module_reload_config(void);

    /**
     * @brief Print module status information
     *
     * Log current status via ESP_LOGI.
     * Useful for debugging.
     *
     * @return esp_err_t Always returns ESP_OK
     */
    esp_err_t smart_voice_control_module_print_status(void);

    /**
     * @brief Push an event into the processing queue
     *
     * Used by external modules (e.g., audio_processor) to send events.
     *
     * @param event Event to push
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_ARG: Invalid argument
     *         - ESP_ERR_TIMEOUT: Queue full
     */
    esp_err_t smart_voice_push_event(const smart_voice_event_t* event);

    /**
     * @brief Register user callback for voice events
     *
     * Register a single callback that receives all events.
     *
     * @param callback Callback function
     * @param user_data User data passed to callback
     * @return esp_err_t Always returns ESP_OK
     */
    esp_err_t smart_voice_register_callback(smart_voice_event_callback_t callback, void* user_data);

    /**
     * @brief Get current operation mode (simplified API)
     *
     * @return smart_voice_mode_t Current mode
     */
    smart_voice_mode_t smart_voice_get_mode(void);

    /**
     * @brief Set operation mode (simplified API)
     *
     * @param mode Target mode
     * @return esp_err_t
     *         - ESP_OK: Success
     *         - ESP_ERR_INVALID_ARG: Invalid mode
     */
    esp_err_t smart_voice_set_mode(smart_voice_mode_t mode);

    /**
     * @brief Reload commands from config file (simplified API)
     *
     * @return esp_err_t
     */
    esp_err_t smart_voice_reload_commands(void);

#else

// Empty implementations when disabled
static inline esp_err_t smart_voice_control_module_init(const smart_voice_config_params_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_control_module_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_control_module_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_control_module_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t
smart_voice_control_module_set_callbacks(const smart_voice_callbacks_t* callbacks)
{
    (void)callbacks;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline smart_voice_mode_t smart_voice_control_module_get_mode(void)
{
    return SMART_VOICE_MODE_UNKNOWN;
}

static inline esp_err_t smart_voice_control_module_set_mode(smart_voice_mode_t mode)
{
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline smart_voice_state_t smart_voice_control_module_get_state(void)
{
    return SMART_VOICE_STATE_UNINIT;
}

static inline smart_voice_network_status_t smart_voice_control_module_get_network_status(void)
{
    return SMART_VOICE_NETWORK_UNKNOWN;
}

static inline bool smart_voice_control_module_is_wake_word_detected(void)
{
    return false;
}

static inline int smart_voice_control_module_get_command_count(void)
{
    return 0;
}

static inline const smart_voice_command_t* smart_voice_control_module_get_command(int index)
{
    (void)index;
    return NULL;
}

static inline const smart_voice_command_t*
smart_voice_control_module_find_command(const char* keyword)
{
    (void)keyword;
    return NULL;
}

static inline esp_err_t smart_voice_control_module_reload_config(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_control_module_print_status(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_push_event(const smart_voice_event_t* event)
{
    (void)event;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_register_callback(smart_voice_event_callback_t callback,
                                                      void*                        user_data)
{
    (void)callback;
    (void)user_data;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline smart_voice_mode_t smart_voice_get_mode(void)
{
    return SMART_VOICE_MODE_UNKNOWN;
}

static inline esp_err_t smart_voice_set_mode(smart_voice_mode_t mode)
{
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t smart_voice_reload_commands(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif // SMART_VOICE_CONTROL_MODULE_H
