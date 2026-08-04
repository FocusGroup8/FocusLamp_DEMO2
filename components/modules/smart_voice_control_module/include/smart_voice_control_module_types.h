#ifndef SMART_VOICE_CONTROL_MODULE_TYPES_H
#define SMART_VOICE_CONTROL_MODULE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Smart Voice Control Module operation mode
     */
    typedef enum
    {
        SMART_VOICE_MODE_LOCAL_ONLY = 0, // Only local command recognition (offline)
        SMART_VOICE_MODE_CLOUD_ONLY,     // Only cloud voice service (online)
        SMART_VOICE_MODE_HYBRID,         // Local priority + cloud fallback (default)
        SMART_VOICE_MODE_UNKNOWN,
    } smart_voice_mode_t;

    /**
     * @brief Smart Voice Control Module state machine states
     */
    typedef enum
    {
        SMART_VOICE_STATE_UNINIT = 0,    // Not initialized
        SMART_VOICE_STATE_INIT,          // Initialized but not started
        SMART_VOICE_STATE_IDLE,          // Idle state, waiting for wake word
        SMART_VOICE_STATE_WAKE_DETECTED, // Wake word detected, listening for command
        SMART_VOICE_STATE_PROCESSING,    // Processing voice input (local or cloud)
        SMART_VOICE_STATE_EXECUTING,     // Executing recognized command/action
        SMART_VOICE_STATE_SPEAKING,      // TTS playing or audio output
        SMART_VOICE_STATE_ERROR,         // Error state
        SMART_VOICE_STATE_UNKNOWN,
    } smart_voice_state_t;

    /**
     * @brief Voice command type classification
     */
    typedef enum
    {
        SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE = 0, // Page navigation (LCD)
        SMART_VOICE_CMD_TYPE_EXPRESSION,        // Expression change (LCD emoji)
        SMART_VOICE_CMD_TYPE_LED_CONTROL,       // LED control
        SMART_VOICE_CMD_TYPE_SERVO_CONTROL,     // Servo control
        SMART_VOICE_CMD_TYPE_LIGHT_CONTROL,     // Light control
        SMART_VOICE_CMD_TYPE_SYSTEM_CMD,        // System commands (reboot, etc.)
        SMART_VOICE_CMD_TYPE_CUSTOM,            // Custom user-defined action
        SMART_VOICE_CMD_TYPE_UNKNOWN,
    } smart_voice_cmd_type_t;

/**
 * @brief Local MultiNet fixed command IDs
 *
 * These IDs correspond to offline commands registered with ESP-SR MultiNet.
 */
#define SMART_VOICE_LOCAL_CMD_SWITCH_PAGE 1
#define SMART_VOICE_LOCAL_CMD_EXPR_HAPPY 2
#define SMART_VOICE_LOCAL_CMD_EXPR_NORMAL 3
#define SMART_VOICE_LOCAL_CMD_EXPR_SLEEPY 4
#define SMART_VOICE_LOCAL_CMD_BLINK_ON 5
#define SMART_VOICE_LOCAL_CMD_BLINK_OFF 6
#define SMART_VOICE_LOCAL_CMD_CLOUD_ON 7

#define SMART_VOICE_LOCAL_CMD_COUNT 7

    /**
     * @brief Command source identification
     */
    typedef enum
    {
        SMART_VOICE_SOURCE_LOCAL = 0, // Local MultiNet recognition
        SMART_VOICE_SOURCE_CLOUD,     // Cloud Xiaozhi service response
        SMART_VOICE_SOURCE_UNKNOWN,
    } smart_voice_source_t;

    /**
     * @brief Text role for cloud responses (user or assistant)
     */
    typedef enum
    {
        SMART_VOICE_TEXT_ROLE_USER = 0,  // User message role
        SMART_VOICE_TEXT_ROLE_ASSISTANT, // Assistant message role
    } smart_voice_text_role_t;

    /**
     * @brief Network status for automatic fallback
     */
    typedef enum
    {
        SMART_VOICE_NETWORK_DISCONNECTED = 0,
        SMART_VOICE_NETWORK_CONNECTED,
        SMART_VOICE_NETWORK_RECONNECTING,
        SMART_VOICE_NETWORK_UNKNOWN,
    } smart_voice_network_status_t;

    /**
     * @brief Event types for smart voice control module
     */
    typedef enum
    {
        SMART_VOICE_EVENT_WAKE_WORD = 0,      // Wake word detected
        SMART_VOICE_EVENT_COMMAND_TEXT,       // Command text recognized (local)
        SMART_VOICE_EVENT_AUDIO_DATA,         // Raw audio data for cloud
        SMART_VOICE_EVENT_LOCAL_CMD,          // Local fixed command matched
        SMART_VOICE_EVENT_CLOUD_TEXT,         // Cloud text response received
        SMART_VOICE_EVENT_CLOUD_EMOJI,        // Cloud emoji command received
        SMART_VOICE_EVENT_TTS_STATE,          // TTS state changed
        SMART_VOICE_EVENT_CMD_NOT_RECOGNIZED, // Command not recognized
        SMART_VOICE_EVENT_ERROR,              // Error occurred
        SMART_VOICE_EVENT_UNKNOWN,
    } smart_voice_event_type_t;

    /**
     * @brief Cloud integration state machine
     */
    typedef enum
    {
        SMART_VOICE_CLOUD_STATE_IDLE = 0,       // Not initialized or idle
        SMART_VOICE_CLOUD_STATE_INITIALIZED,    // Initialized but not started
        SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE, // Pre-connected (WebSocket up) but not listening
        SMART_VOICE_CLOUD_STATE_RUNNING, // Connected and running (audio channel open + listening)
        SMART_VOICE_CLOUD_STATE_STOPPED, // Stopped (audio channel closed)
        SMART_VOICE_CLOUD_STATE_DISCONNECTED, // WebSocket disconnected unexpectedly
        SMART_VOICE_CLOUD_STATE_ERROR,        // Error state
        SMART_VOICE_CLOUD_STATE_UNKNOWN,
    } smart_voice_cloud_state_t;

    /**
     * @brief Fixed voice command definition (from JSON config)
     */
    typedef struct
    {
        int                    id;               // Unique command ID
        char                   keyword[64];      // Trigger word/phrase
        char                   description[128]; // Human-readable description
        smart_voice_cmd_type_t type;             // Command type
        char                   action[256];      // Action string (JSON format for custom actions)
        int                    priority;         // Execution priority (lower = higher priority)
        bool                   enabled;          // Whether this command is enabled
    } smart_voice_command_t;

    /**
     * @brief Event data union for different event types
     */
    typedef struct
    {
        // Wake word event data
        struct
        {
            const char* wake_word;
        } wake_word;

        // Command text event data (from local speech recognition)
        struct
        {
            const char* text;
        } command_text;

        // Audio data event data
        struct
        {
            uint8_t* data;
            size_t   length;
        } audio;

        // Local command matched event data
        struct
        {
            const smart_voice_command_t* command;
        } local_cmd;

        // Command keyword/action (for system commands or simple matches)
        struct
        {
            const char* keyword;
            const char* action;
        } command;

        // Cloud text response event data
        struct
        {
            const char* text;
            int         role; // SMART_VOICE_TEXT_ROLE_USER or ASSISTANT
        } cloud_text;

        // Cloud emoji event data
        struct
        {
            const char* emoji_str; // Emoji string from server
        } emoji;

        // TTS state event data
        struct
        {
            int         state; // TTS state value
            const char* text;  // Text for sentence_start, NULL otherwise
        } tts;

        // Unrecognized command event data
        struct
        {
            const char* text;
        } unrecognized;

        // Error event data
        struct
        {
            int         code;    // Error code
            const char* message; // Error message/description
        } error;
    } smart_voice_event_data_t;

    /**
     * @brief Main event structure for smart voice control module
     */
    typedef struct
    {
        smart_voice_event_type_t type;      // Event type
        smart_voice_event_data_t data;      // Event-specific data
        uint32_t                 timestamp; // Event timestamp (ms)
    } smart_voice_event_t;

    /**
     * @brief Configuration loaded from JSON file
     */
    typedef struct
    {
        smart_voice_command_t* commands;      // Array of fixed commands
        int                    command_count; // Number of commands
        char                   version[32];   // Config file version
        uint32_t               last_modified; // Last modification timestamp
    } smart_voice_config_t;

    /**
     * @brief Main module configuration structure
     */
    typedef struct
    {
        smart_voice_mode_t mode;                 // Operation mode (LOCAL_ONLY, CLOUD_ONLY, HYBRID)
        const char*        wake_word;            // Wake word string
        int                task_stack_size;      // Task stack size in bytes
        int                task_priority;        // FreeRTOS task priority
        bool               enable_aec;           // Enable AEC
        bool               enable_vad;           // Enable VAD
        bool               enable_wake_word;     // Enable wake word detection
        const char*        wake_word_model;      // Wake word model name
        bool               auto_fallback;        // Enable network auto-fallback
        int                cloud_retry_count;    // Cloud retry count
        int                cloud_retry_delay_ms; // Cloud retry delay in ms
        const char*        config_path;          // Config file path
        bool               enable_hot_reload;    // Enable config hot reload
    } smart_voice_config_params_t;

    /**
     * @brief Callback function types
     */

    // Audio output callback from processor (processed PCM data)
    typedef void (*smart_voice_audio_output_callback_t)(const int16_t* data, size_t size,
                                                        void* user_data);

    // VAD state change callback
    typedef void (*smart_voice_vad_callback_t)(bool is_speaking, void* user_data);

    // Wake word detected callback
    typedef void (*smart_voice_wake_word_callback_t)(const char* wake_word, void* user_data);

    // Local command recognized callback (from JSON config / keyword match)
    typedef void (*smart_voice_local_command_callback_t)(const smart_voice_command_t* command,
                                                         void*                        user_data);

    // Local MultiNet command recognized callback (from ESP-SR offline recognition)
    typedef esp_err_t (*smart_voice_multinet_cmd_callback_t)(int command_id, const char* keyword,
                                                             void* user_data);

    // Cloud response received callback
    typedef void (*smart_voice_cloud_response_callback_t)(const char* text, const char* emoji,
                                                          void* user_data);

    // TTS state change callback
    typedef void (*smart_voice_tts_state_callback_t)(bool is_speaking, const char* text,
                                                     void* user_data);

    // State change callback
    typedef void (*smart_voice_state_change_callback_t)(smart_voice_state_t old_state,
                                                        smart_voice_state_t new_state,
                                                        void*               user_data);

    // Mode change callback
    typedef void (*smart_voice_mode_change_callback_t)(smart_voice_mode_t old_mode,
                                                       smart_voice_mode_t new_mode,
                                                       void*              user_data);

    // Network status change callback
    typedef void (*smart_voice_network_status_callback_t)(smart_voice_network_status_t status,
                                                          void*                        user_data);

    // Error callback
    typedef void (*smart_voice_error_callback_t)(esp_err_t error_code, const char* error_msg,
                                                 void* user_data);

    // Generic event callback (used for main module event notification)
    typedef void (*smart_voice_event_callback_t)(const smart_voice_event_t* event, void* user_data);

    // Cloud integration callback (internal use)
    typedef void (*smart_voice_cloud_callback_t)(const smart_voice_event_t* event, void* user_data);

    /**
     * @brief Callbacks collection structure
     */
    typedef struct
    {
        smart_voice_audio_output_callback_t   on_audio_output;
        smart_voice_vad_callback_t            on_vad_change;
        smart_voice_wake_word_callback_t      on_wake_word;
        smart_voice_local_command_callback_t  on_local_command;
        smart_voice_multinet_cmd_callback_t   on_multinet_command;
        smart_voice_cloud_response_callback_t on_cloud_response;
        smart_voice_tts_state_callback_t      on_tts_state;
        smart_voice_state_change_callback_t   on_state_change;
        smart_voice_mode_change_callback_t    on_mode_change;
        smart_voice_network_status_callback_t on_network_status;
        smart_voice_error_callback_t          on_error;
    } smart_voice_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif // SMART_VOICE_CONTROL_MODULE_TYPES_H
