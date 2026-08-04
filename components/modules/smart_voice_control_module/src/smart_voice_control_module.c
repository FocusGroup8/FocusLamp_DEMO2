#include "smart_voice_control_module.h"

#include <string.h>

#include "esp_heap_caps.h" // For SPIRAM allocation
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_driver.h"
#include "audio_i2s.h"
#include "audio_processor.h"
#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"

#if (EVENT_BUS_ENABLE == 1)
#include "event_bus.h"
#endif

#if (SMART_VOICE_ENABLE_MULTINET == 1)
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#endif

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "smart_voice";

// Module state
static smart_voice_mode_t s_current_mode = SMART_VOICE_MODE_HYBRID;
static bool               s_initialized  = false;

// Wake word state machine
typedef enum
{
    WAKE_STATE_IDLE = 0,           // Waiting for wake word (only wake word detection active)
    WAKE_STATE_DETECTED,           // Wake word detected, starting cloud session
    WAKE_STATE_LOCAL_RECOGNIZING,  // Wake word detected, trying local MultiNet recognition
                                   // (VAD-triggered)
    WAKE_STATE_LISTENING,          // Listening for user speech (sending audio to cloud)
    WAKE_STATE_CONVERSING,         // AI is responding
    WAKE_STATE_WAITING_FOR_SPEECH, // AI finished, waiting for user to speak (VAD + timeout)
} wake_state_t;
static volatile wake_state_t s_wake_state  = WAKE_STATE_IDLE;
static SemaphoreHandle_t     s_state_mutex = NULL; // Mutex for protecting s_wake_state
static volatile bool         s_session_ended =
    false; // Flag: session ended by keyword (ignore subsequent TTS)

#if (SMART_VOICE_ENABLE_MULTINET == 1)
static int16_t* s_multinet_buffer = NULL;
static int      s_multinet_index  = 0;
#define MULTINET_BUFFER_SIZE 512
static const esp_mn_iface_t* s_multinet                 = NULL;
static model_iface_data_t*   s_mn_data                  = NULL;
static TickType_t            s_local_recog_start_tick   = 0;
static bool                  s_multinet_speech_detected = false;
#endif

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
static int16_t* s_wakenet_buffer = NULL;
#endif

// Safe state access functions
// Note: Reading uses direct volatile access (lock-free) for high-frequency calls in audio loop.
// Writing uses mutex to ensure atomic state transitions.
static inline wake_state_t get_wake_state(void)
{
    return s_wake_state; // Direct volatile read - safe for single variable on RISC-V
}

static inline void set_wake_state(wake_state_t new_state)
{
    if (s_state_mutex != NULL && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_wake_state = new_state;
        xSemaphoreGive(s_state_mutex);
    }
    else
    {
        s_wake_state = new_state; // Fallback if mutex unavailable
    }
}

// Session timeout for auto-reset (in ms)
#define SESSION_TIMEOUT_MS (30000) // 30 seconds timeout

// Silence timeout in WAITING state (ms) - if no speech detected, go back to IDLE
#define SILENCE_TIMEOUT_MS (4000)             // 4 seconds silence timeout
static TickType_t s_session_start_tick   = 0; // When non-IDLE state started
static TickType_t s_waiting_start_tick   = 0; // When WAITING state started (for silence timeout)
static bool s_speech_detected_in_waiting = false; // Track if speech was detected while waiting

// Send tracking variables (global for access across functions)
static uint32_t   consecutive_send_failures = 0; // Track consecutive send failures
static TickType_t last_successful_send      = 0; // Last time we successfully sent audio

// Async cloud send queue (prevents audio process task from blocking on WebSocket)
#define CLOUD_SEND_QUEUE_LENGTH 4 // Max pending send requests (reduced to save memory)
#define OPUS_FRAME_SIZE 960       // 60ms @ 16kHz (方案J: reverted from 1920 for smooth playback)

// Allocation limit to prevent memory exhaustion during cloud dialogue
// Increased from 4 to 8 (方案H: allow more concurrent allocations for smoother audio flow)
#define MAX_ACTIVE_ALLOCATIONS 8              // Max simultaneous malloc (8 * 3840B = ~30KB)
static volatile int s_active_allocations = 0; // Track current active allocations

// Use pointer-based queue (with allocation count limiting)
typedef struct
{
    int16_t* pcm_data;   // Pointer to PCM data (dynamically allocated)
    size_t   pcm_length; // Number of samples
} cloud_send_item_t;

static QueueHandle_t s_cloud_send_queue        = NULL; // Queue for async cloud sends
static TaskHandle_t  s_cloud_send_task_handle  = NULL;
static bool          s_cloud_send_task_running = false;

// Task handle
static TaskHandle_t  s_task_handle            = NULL;
static TaskHandle_t  s_i2s_reader_task_handle = NULL;
static bool          s_i2s_reader_running     = false;
static volatile bool s_pending_cloud_start    = false;
static volatile bool s_pending_multinet_reload =
    false; // 方案P-L: Flag for delayed reload on wake word
static TaskHandle_t  s_audio_process_task_handle  = NULL;
static bool          s_audio_process_task_running = false;
static QueueHandle_t s_pcm_queue   = NULL; // Queue for I2S → Process task communication
static QueueHandle_t s_event_queue = NULL;

// Audio processor for wake word detection (WakeNet)
static audio_processor_t* s_audio_processor = NULL;
static int                s_feed_chunksize  = 0;

// Configuration
static smart_voice_config_params_t s_config = {0};

// Callbacks
static smart_voice_event_callback_t s_user_callback = NULL;
static void*                        s_user_data     = NULL;

// Forward declarations for sub-module functions
esp_err_t                    voice_command_parser_init(void);
void                         voice_command_parser_deinit(void);
const smart_voice_command_t* voice_command_find_by_keyword(const char* keyword);
int                          voice_command_get_count(void);
esp_err_t                    voice_command_reload_config(void);

esp_err_t network_monitor_init(void);
void      network_monitor_deinit(void);
bool      network_monitor_is_connected(void);
esp_err_t network_monitor_set_callback(smart_voice_network_status_callback_t callback,
                                       void*                                 user_data);
void      network_monitor_print_status(void);

esp_err_t                 cloud_integration_init(void);
void                      cloud_integration_deinit(void);
esp_err_t                 cloud_integration_start(void);
esp_err_t                 cloud_integration_preconnect(void);
esp_err_t                 cloud_integration_stop(void);
bool                      cloud_integration_is_ready(void);
smart_voice_cloud_state_t cloud_integration_get_state(void);
esp_err_t                 cloud_integration_send_audio(const int16_t* pcm_data, size_t pcm_len);
esp_err_t                 cloud_integration_send_wake_word(const char* wake_word);
esp_err_t                 cloud_integration_activate(void);
esp_err_t        cloud_integration_check_reconnect(void); // Auto-reconnect on WebSocket disconnect
static esp_err_t ensure_cloud_send_task(void);
esp_err_t cloud_integration_set_callback(smart_voice_cloud_callback_t callback, void* user_data);
void      cloud_integration_print_status(void);

/**
 * @brief Handle network status change
 */
static void on_network_status_changed(smart_voice_network_status_t status, void* user_data)
{
    (void)user_data;

    ESP_LOGI(TAG, "Network status changed: %d", status);

    if (status == SMART_VOICE_NETWORK_CONNECTED)
    {
        // Network recovered, try to switch back to hybrid mode
        if (s_current_mode == SMART_VOICE_MODE_LOCAL_ONLY)
        {
            esp_err_t ret = cloud_integration_start();
            if (ret == ESP_OK)
            {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_current_mode = SMART_VOICE_MODE_HYBRID;
                xSemaphoreGive(s_state_mutex);

                ESP_LOGI(TAG, "Switched back to HYBRID mode");
            }
        }
    }
    else if (status == SMART_VOICE_NETWORK_DISCONNECTED)
    {
        // Network lost, fallback to local only
        cloud_integration_stop();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_current_mode = SMART_VOICE_MODE_LOCAL_ONLY;
        xSemaphoreGive(s_state_mutex);

        ESP_LOGW(TAG, "Fallback to LOCAL_ONLY mode");
    }
}

// Session end keywords (user says goodbye or ends conversation)
static const char* s_session_end_keywords[] = {"再见", "拜拜",   "bye",    "goodbye", "结束",
                                               "挂断", "没事了", "不用了", "好了",    "ok"};
#define SESSION_END_KEYWORD_COUNT \
    (sizeof(s_session_end_keywords) / sizeof(s_session_end_keywords[0]))

/**
 * @brief Check if TTS text contains session-end keywords
 * @return true if conversation should end (go back to IDLE)
 */
static bool is_session_end_text(const char* text)
{
    if (text == NULL || strlen(text) == 0)
    {
        return false;
    }

    for (int i = 0; i < SESSION_END_KEYWORD_COUNT; i++)
    {
        if (strcasestr(text, s_session_end_keywords[i]) != NULL)
        {
            ESP_LOGI(TAG, "Session end keyword detected: '%s' in text: '%s'",
                     s_session_end_keywords[i], text);
            return true;
        }
    }
    return false;
}

/**
 * @brief VAD callback - called when voice activity state changes
 *
 * In WAITING_FOR_SPEECH state:
 * - We NO LONGER auto-transition to LISTENING on speech detection
 * - User MUST use wake word to start new conversation
 * - SILENCE timeout will handle transition back to IDLE
 */
static void on_vad_state_changed(audio_processor_vad_state_t vad_state, void* user_data)
{
    (void)user_data;

    // VAD logging only (no state transitions in WAITING mode)
    if (get_wake_state() == WAKE_STATE_WAITING_FOR_SPEECH)
    {
        if (vad_state == AUDIO_PROCESSOR_VAD_SPEECH)
        {
            ESP_LOGD(TAG, "VAD: Speech detected in WAITING state (ignoring - requires wake word)");
            // NOTE: We do NOT transition to LISTENING here
            // User must use wake word to start new conversation
        }
        // SILENCE is expected - timeout will handle transition back to IDLE
    }
}

/**
 * @brief Handle cloud event
 */
static void on_cloud_event(const smart_voice_event_t* event, void* user_data)
{
    (void)user_data;

    if (event == NULL)
    {
        return;
    }

    // Handle TTS state changes for continuous conversation mode
    if (event->type == SMART_VOICE_EVENT_TTS_STATE)
    {
        int          tts_state     = event->data.tts.state;
        wake_state_t current_state = get_wake_state();
        ESP_LOGD(TAG, "TTS state: %d, text: %s, current_wake_state=%d", tts_state,
                 event->data.tts.text ? event->data.tts.text : "", current_state);

        // TTS state: 0=IDLE, 1=START, 2=PLAYING, 3=END
        if (tts_state == 1 || tts_state == 2)
        {
            // START or PLAYING - AI is responding
            const char* tts_text = event->data.tts.text;

            // 🔑 IMPORTANT: If session was already ended by keyword, ignore all subsequent TTS
            // events! This prevents state from being changed back to CONVERSING after "再见"
            // detection
            if (s_session_ended)
            {
                ESP_LOGD(TAG, "Session already ended, ignoring TTS event (state=%d, text='%s')",
                         tts_state, tts_text ? tts_text : "(null)");
                return; // Ignore this event completely
            }

            // 🔑 IMPORTANT: Check for session-end keywords during PLAYING state!
            // This handles cases where:
            // 1. WebSocket disconnects before TTS END event
            // 2. User said "再见" and AI is responding with goodbye message
            //    (state may be WAITING_FOR_SPEECH instead of CONVERSING)
            if (tts_state == 2 && is_session_end_text(tts_text))
            {
                // Session end keyword detected during TTS playing - end session immediately
                ESP_LOGI(
                    TAG,
                    "Session end keyword detected during TTS PLAYING: '%s'. Wake state: %d → IDLE",
                    tts_text ? tts_text : "(null)", current_state);
                set_wake_state(WAKE_STATE_IDLE);
                s_session_start_tick = 0;
                s_session_ended      = true; // Mark session as ended - ignore subsequent TTS

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                if (s_audio_processor != NULL)
                {
                    audio_processor_reset(s_audio_processor);
                }
#endif

                // Reload MultiNet after cloud session ends (async in main task)
#if (SMART_VOICE_ENABLE_MULTINET == 1)
                s_pending_multinet_reload = true;
                ESP_LOGI(TAG, "Scheduling MultiNet reload after cloud session end...");
#endif
            }
            else
            {
                // Normal response - AI is talking
                if (current_state != WAKE_STATE_CONVERSING && current_state != WAKE_STATE_LISTENING)
                {
                    s_session_start_tick = xTaskGetTickCount(); // Record session start time
                }
                // Set to CONVERSING to indicate AI is talking, but don't stop sending audio!
                set_wake_state(WAKE_STATE_CONVERSING);
            }
        }
        else if (tts_state == 0 || tts_state == 3)
        {
            // TTS IDLE or END - AI finished responding

            // If session already ended, ignore this event
            if (s_session_ended)
            {
                ESP_LOGD(TAG, "Session already ended, ignoring TTS END event");
                return;
            }

            // IMPORTANT: Check if this is a session-ending response!
            current_state = get_wake_state(); // Re-read as it may have changed
            if (current_state == WAKE_STATE_CONVERSING)
            {
                const char* tts_text = event->data.tts.text;

                if (is_session_end_text(tts_text))
                {
                    // Session end keyword detected - go back to IDLE (require wake word)
                    ESP_LOGI(TAG, "Session ended (keyword in: '%s'). Wake state: %d → IDLE",
                             tts_text ? tts_text : "(null)", current_state);
                    set_wake_state(WAKE_STATE_IDLE);
                    s_session_start_tick = 0;
                    s_session_ended      = true; // Mark session as ended

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                    if (s_audio_processor != NULL)
                    {
                        audio_processor_reset(s_audio_processor);
                    }
#endif

                    // Reload MultiNet after cloud session ends (async in main task)
#if (SMART_VOICE_ENABLE_MULTINET == 1)
                    s_pending_multinet_reload = true;
                    ESP_LOGI(TAG, "Scheduling MultiNet reload after cloud session end...");
#endif
                }
                else
                {
                    // Normal response - enter WAITING state (VAD + silence timeout)
                    current_state = get_wake_state();
                    ESP_LOGI(TAG,
                             "AI response ended. Wake state: %d → WAITING_FOR_SPEECH (VAD mode)",
                             current_state);
                    set_wake_state(WAKE_STATE_WAITING_FOR_SPEECH);
                    s_waiting_start_tick         = xTaskGetTickCount(); // Start silence timeout
                    s_speech_detected_in_waiting = false;

                    // 🔑 CRITICAL: Immediately schedule MultiNet reload to free memory!
                    // Don't wait for 4s silence timeout - memory is already tight
#if (SMART_VOICE_ENABLE_MULTINET == 1)
                    s_pending_multinet_reload = true;
                    ESP_LOGI(TAG, "⚡ Immediate MultiNet reload scheduled (entering WAITING)");
#endif

                    // Reset send timers for new waiting period
                    last_successful_send      = xTaskGetTickCount();
                    consecutive_send_failures = 0;

                    // Reset AFE/WakeNet internal buffer for next wake word detection
                    // (in case user wants to use wake word to start new topic)
#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                    if (s_audio_processor != NULL)
                    {
                        audio_processor_reset(s_audio_processor);
                    }
#endif
                }
            }
        }
    }

    // Forward to user callback
    if (s_user_callback != NULL && event != NULL)
    {
        s_user_callback(event, s_user_data);
    }
}

/**
 * @brief Wake word callback from audio_processor (WakeNet)
 * Called when wake word is detected by ESP-SR
 */

#if (SMART_VOICE_ENABLE_MULTINET == 1)

static esp_err_t multinet_init(void)
{
    srmodel_list_t* models = esp_srmodel_init("model");
    if (models == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize SR model list");
        return ESP_ERR_NOT_FOUND;
    }

    s_multinet = esp_mn_handle_from_name(SMART_VOICE_MULTINET_MODEL);
    if (s_multinet == NULL)
    {
        ESP_LOGE(TAG, "MultiNet model '%s' not found", SMART_VOICE_MULTINET_MODEL);
        return ESP_ERR_NOT_FOUND;
    }

    s_mn_data = s_multinet->create(SMART_VOICE_MULTINET_MODEL, 6000);
    if (s_mn_data == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MultiNet model data");
        return ESP_ERR_NO_MEM;
    }

    esp_mn_commands_alloc(s_multinet, s_mn_data);

    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_SWITCH_PAGE, "qie huan ye mian");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_HAPPY, "xian shi kai xin");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_NORMAL, "xian shi zheng chang");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_SLEEPY, "xian shi shui mian");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_BLINK_ON, "kai qi zha yan");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_BLINK_OFF, "guan bi zha yan");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_CLOUD_ON, "kai qi liao tian");

    esp_mn_commands_update();

    // 方案P-O: Allocate PCM buffer in PSRAM to save Internal RAM (~2KB)
    // Note: Model data (s_mn_data ~50KB) must stay in Internal RAM (ESP-SR requirement)
    if (s_multinet_buffer == NULL)
    {
        s_multinet_buffer =
            (int16_t*)heap_caps_malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_multinet_buffer == NULL)
        {
            ESP_LOGW(
                TAG,
                "[PSRAM] Failed to allocate MultiNet buffer in PSRAM, fallback to Internal RAM");
            s_multinet_buffer = (int16_t*)malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t));
        }
        else
        {
            ESP_LOGI(TAG, "[PSRAM] ✅ MultiNet buffer allocated in PSRAM (%d bytes)",
                     MULTINET_BUFFER_SIZE * sizeof(int16_t));
        }
    }
    s_multinet_index = 0;

    ESP_LOGI(TAG, "MultiNet initialized with %d local commands", SMART_VOICE_LOCAL_CMD_COUNT);
    return ESP_OK;
}

static void multinet_deinit(void)
{
    if (s_mn_data != NULL && s_multinet != NULL)
    {
        s_multinet->destroy(s_mn_data);
        s_mn_data  = NULL;
        s_multinet = NULL;
        ESP_LOGI(TAG, "MultiNet deinitialized");
    }
}

/**
 * @brief Completely unload MultiNet to free memory for cloud operations
 *
 * This function:
 * 1. Frees the MultiNet PCM buffer (~8KB)
 * 2. Destroys the MultiNet model data (~50KB)
 * 3. Total memory freed: ~58KB
 *
 * Call this before starting cloud TLS handshake to avoid memory conflicts.
 */
static esp_err_t multinet_unload(void)
{
    size_t total_freed = 0;

    // Step 1: Free PCM buffer
    if (s_multinet_buffer != NULL)
    {
        free(s_multinet_buffer);
        s_multinet_buffer = NULL;
        total_freed += MULTINET_BUFFER_SIZE * sizeof(int16_t);
        ESP_LOGI(TAG, "MultiNet buffer freed (%d bytes)", MULTINET_BUFFER_SIZE * sizeof(int16_t));
    }
    s_multinet_index = 0;

    // Step 2: Destroy model data
    if (s_mn_data != NULL && s_multinet != NULL)
    {
        s_multinet->destroy(s_mn_data);
        s_mn_data  = NULL;
        s_multinet = NULL;
        total_freed += 50000; // Approximate model size
        ESP_LOGI(TAG, "MultiNet model destroyed");
    }

    ESP_LOGI(TAG, "✅ MultiNet completely unloaded (freed ~%d bytes)", total_freed);
    return ESP_OK;
}

/**
 * @brief Reload MultiNet after cloud session ends
 *
 * This function re-initializes MultiNet with the same configuration as init.
 * Call this after stopping cloud connection to restore offline recognition.
 *
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t multinet_reload(void)
{
    ESP_LOGI(TAG, "Reloading MultiNet for offline recognition...");

    // Check if already loaded
    if (s_mn_data != NULL && s_multinet != NULL)
    {
        ESP_LOGW(TAG, "MultiNet already loaded, skipping reload");
        return ESP_OK;
    }

    // Step 1: Re-initialize SR model list
    srmodel_list_t* models = esp_srmodel_init("model");
    if (models == NULL)
    {
        ESP_LOGE(TAG, "Failed to re-initialize SR model list");
        return ESP_ERR_NOT_FOUND;
    }

    // Step 2: Get model handle
    s_multinet = esp_mn_handle_from_name(SMART_VOICE_MULTINET_MODEL);
    if (s_multinet == NULL)
    {
        ESP_LOGE(TAG, "MultiNet model '%s' not found during reload", SMART_VOICE_MULTINET_MODEL);
        return ESP_ERR_NOT_FOUND;
    }

    // Step 3: Create model data (~50KB in Internal RAM - ESP-SR requirement)
    s_mn_data = s_multinet->create(SMART_VOICE_MULTINET_MODEL, 6000);
    if (s_mn_data == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MultiNet model data during reload");
        s_multinet = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Step 4: Allocate and configure commands
    esp_mn_commands_alloc(s_multinet, s_mn_data);

    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_SWITCH_PAGE, "qie huan ye mian");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_HAPPY, "xian shi kai xin");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_NORMAL, "xian shi zheng chang");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_EXPR_SLEEPY, "xian shi shui mian");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_BLINK_ON, "kai qi zha yan");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_BLINK_OFF, "guan bi zha yan");
    esp_mn_commands_add(SMART_VOICE_LOCAL_CMD_CLOUD_ON, "kai qi liao tian");

    esp_mn_commands_update();

    // Step 5: Allocate PCM buffer in PSRAM (方案P-O: save Internal RAM)
    if (s_multinet_buffer == NULL)
    {
        s_multinet_buffer =
            (int16_t*)heap_caps_malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_multinet_buffer == NULL)
        {
            ESP_LOGW(TAG, "[PSRAM] Failed to allocate buffer in PSRAM during reload, fallback to "
                          "Internal RAM");
            s_multinet_buffer = (int16_t*)malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t));
        }
        else
        {
            ESP_LOGI(TAG, "[PSRAM] ✅ MultiNet buffer allocated in PSRAM during reload (%d bytes)",
                     MULTINET_BUFFER_SIZE * sizeof(int16_t));
        }
    }
    if (s_multinet_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate MultiNet buffer during reload");
        s_multinet->destroy(s_mn_data);
        s_mn_data  = NULL;
        s_multinet = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_multinet_index = 0;

    ESP_LOGI(TAG, "✅ MultiNet reloaded successfully (%d local commands ready)",
             SMART_VOICE_LOCAL_CMD_COUNT);
    return ESP_OK;
}

static const char* get_local_cmd_keyword(int cmd_id)
{
    switch (cmd_id)
    {
    case SMART_VOICE_LOCAL_CMD_SWITCH_PAGE:
        return "切换页面";
    case SMART_VOICE_LOCAL_CMD_EXPR_HAPPY:
        return "显示开心";
    case SMART_VOICE_LOCAL_CMD_EXPR_NORMAL:
        return "显示正常";
    case SMART_VOICE_LOCAL_CMD_EXPR_SLEEPY:
        return "显示睡眠";
    case SMART_VOICE_LOCAL_CMD_BLINK_ON:
        return "开启眨眼";
    case SMART_VOICE_LOCAL_CMD_BLINK_OFF:
        return "关闭眨眼";
    case SMART_VOICE_LOCAL_CMD_CLOUD_ON:
        return "开启聊天";
    default:
        return "未知";
    }
}

static void free_local_recognition_buffers(void)
{
    size_t total_freed = 0;

#if (SMART_VOICE_ENABLE_MULTINET == 1)
    if (s_multinet_buffer != NULL)
    {
        free(s_multinet_buffer);
        s_multinet_buffer = NULL;
        s_multinet_index  = 0;
        total_freed += MULTINET_BUFFER_SIZE * sizeof(int16_t);
        ESP_LOGI(TAG, "MultiNet buffer freed");
    }
#endif

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
    if (s_wakenet_buffer != NULL)
    {
        free(s_wakenet_buffer);
        s_wakenet_buffer = NULL;
        total_freed += s_feed_chunksize * sizeof(int16_t);
        ESP_LOGI(TAG, "WakeNet buffer freed");
    }
#endif

    if (total_freed > 0)
    {
        ESP_LOGI(TAG, "Local recognition buffers freed (%d bytes total)", (int)total_freed);
    }
}

static esp_err_t __attribute__((unused)) ensure_cloud_started(void)
{
    smart_voice_cloud_state_t cstate = cloud_integration_get_state();
    if (cstate == SMART_VOICE_CLOUD_STATE_RUNNING)
    {
        return ESP_OK;
    }
    if (cstate == SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE)
    {
        ESP_LOGI(TAG, "Cloud preconnected, activating...");
        return cloud_integration_activate();
    }
    if (!network_monitor_is_connected())
    {
        ESP_LOGW(TAG, "Cannot start cloud: network not connected");
        return ESP_ERR_NOT_SUPPORTED;
    }

    free_local_recognition_buffers();

    esp_err_t ret = cloud_integration_start();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to start cloud on-demand: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Cloud session started on-demand");
    }
    return ret;
}

static esp_err_t execute_local_command(int cmd_id)
{
    ESP_LOGI(TAG, "🎤 Local command matched! ID=%d keyword='%s'", cmd_id,
             get_local_cmd_keyword(cmd_id));

    switch (cmd_id)
    {
    case SMART_VOICE_LOCAL_CMD_SWITCH_PAGE:
        ESP_LOGI(TAG, "[Local] Executing: 切换页面 (LCD page switch)");
        break;

    case SMART_VOICE_LOCAL_CMD_EXPR_HAPPY:
        ESP_LOGI(TAG, "[Local] Executing: 显示开心 (expression happy)");
        break;

    case SMART_VOICE_LOCAL_CMD_EXPR_NORMAL:
        ESP_LOGI(TAG, "[Local] Executing: 显示正常 (expression normal)");
        break;

    case SMART_VOICE_LOCAL_CMD_EXPR_SLEEPY:
        ESP_LOGI(TAG, "[Local] Executing: 显示睡眠 (expression sleepy)");
        break;

    case SMART_VOICE_LOCAL_CMD_BLINK_ON:
        ESP_LOGI(TAG, "[Local] Executing: 开启眨眼 (blink enable)");
        break;

    case SMART_VOICE_LOCAL_CMD_BLINK_OFF:
        ESP_LOGI(TAG, "[Local] Executing: 关闭眨眼 (blink disable)");
        break;

    case SMART_VOICE_LOCAL_CMD_CLOUD_ON:
        ESP_LOGI(TAG, "[Local] Executing: 开启聊天 → Requesting cloud AI mode");
        s_current_mode        = SMART_VOICE_MODE_HYBRID;
        s_pending_cloud_start = true;
        ESP_LOGI(TAG, "Cloud start requested - will be handled by main task in safe context");
        set_wake_state(WAKE_STATE_LISTENING);
        s_session_start_tick = xTaskGetTickCount();
        s_session_ended      = false;
        return ESP_OK;

    default:
        ESP_LOGW(TAG, "Unknown local command ID: %d", cmd_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_user_callback != NULL)
    {
        smart_voice_event_t event;
        event.type = SMART_VOICE_EVENT_LOCAL_CMD;
        memset(&event.data, 0, sizeof(event.data));
        s_user_callback(&event, s_user_data);
    }

    return ESP_OK;
}

#endif

static void on_wake_word_detected(int wake_word_index, const char* wake_word_name, void* user_data)
{
    (void)wake_word_index;
    (void)wake_word_name;
    (void)user_data;

    // Only process wake word if in IDLE state (ignore during conversation)
    wake_state_t current_state = get_wake_state();
    if (current_state != WAKE_STATE_IDLE)
    {
        ESP_LOGD(TAG, "Wake word detected but state is %d, ignoring", current_state);
        return;
    }

    ESP_LOGI(TAG, "🎯 Wake word detected! Starting cloud session...");

#if (EVENT_BUS_ENABLE == 1)
    voice_wake_word_data_t wake_data = {
        .wake_word  = SMART_VOICE_WAKE_WORD,
        .confidence = 1.0f,
    };
    event_bus_publish_simple(EVENT_TYPE_VOICE_COMMAND, VOICE_EVENT_WAKE_WORD_DETECTED, &wake_data,
                             sizeof(wake_data));
#endif

    // Update state to DETECTED
    set_wake_state(WAKE_STATE_DETECTED);

    // Notify user about wake word
    if (s_user_callback != NULL)
    {
        smart_voice_event_t event;
        event.type                     = SMART_VOICE_EVENT_WAKE_WORD;
        event.data.wake_word.wake_word = SMART_VOICE_WAKE_WORD;
        s_user_callback(&event, s_user_data);
    }

#if (SMART_VOICE_ENABLE_MULTINET == 1)
    // 方案P-L: Delayed MultiNet reload on wake word detection
    // After cloud session ends, we set s_pending_multinet_reload = true
    // but don't actually reload (memory is too tight ~22KB)
    // Now user said wake word → no cloud connection → ~230KB available >> 50KB needed ✅
    if (s_pending_multinet_reload)
    {
        ESP_LOGI(TAG, "🔄 方案P-L: Delayed MultiNet reload triggered by wake word");
        ESP_LOGI(TAG, "   Memory state: No cloud connection → ~230KB available for reload");

        esp_err_t reload_ret = multinet_reload();
        if (reload_ret == ESP_OK)
        {
            s_pending_multinet_reload = false;
            ESP_LOGI(TAG, "✅ 方案P-L: MultiNet reloaded successfully - offline commands ready");
        }
        else
        {
            ESP_LOGE(TAG, "❌ 方案P-L: Failed to reload MultiNet: %s", esp_err_to_name(reload_ret));
            ESP_LOGE(TAG, "   Offline voice commands will be unavailable this session");
            s_pending_multinet_reload = false; // Clear flag to avoid retry loop
        }
    }

    if (s_current_mode == SMART_VOICE_MODE_HYBRID || s_current_mode == SMART_VOICE_MODE_LOCAL_ONLY)
    {
        // Check if MultiNet is loaded before entering LOCAL_RECOGNIZING
        if (s_mn_data != NULL && s_multinet != NULL)
        {
            ESP_LOGI(TAG,
                     "Wake state: IDLE → LOCAL_RECOGNIZING (waiting for local command via VAD)");

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
            if (s_wakenet_buffer == NULL)
            {
                s_wakenet_buffer = (int16_t*)malloc(s_feed_chunksize * sizeof(int16_t));
                if (s_wakenet_buffer != NULL)
                {
                    ESP_LOGI(TAG, "WakeNet buffer reallocated (%d bytes)",
                             s_feed_chunksize * (int)sizeof(int16_t));
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to reallocate WakeNet buffer");
                }
            }
#endif

            if (s_multinet_buffer == NULL)
            {
                // Try PSRAM first (方案P-O), fallback to Internal RAM
                s_multinet_buffer = (int16_t*)heap_caps_malloc(
                    MULTINET_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (s_multinet_buffer == NULL)
                {
                    s_multinet_buffer = (int16_t*)malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t));
                }
                if (s_multinet_buffer != NULL)
                {
                    ESP_LOGI(TAG, "MultiNet buffer reallocated (%d bytes)",
                             MULTINET_BUFFER_SIZE * (int)sizeof(int16_t));
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to reallocate MultiNet buffer");
                }
            }
            set_wake_state(WAKE_STATE_LOCAL_RECOGNIZING);
            s_local_recog_start_tick   = xTaskGetTickCount();
            s_multinet_speech_detected = false;
            return;
        }
        else
        {
            // MultiNet not loaded - skip local recognition, go directly to cloud or idle
            ESP_LOGW(TAG, "MultiNet not loaded - skipping LOCAL_RECOGNIZING state");
            if (s_current_mode == SMART_VOICE_MODE_LOCAL_ONLY)
            {
                ESP_LOGW(TAG, "Local only mode but no MultiNet - staying in IDLE");
                set_wake_state(WAKE_STATE_IDLE);
                return;
            }
            // Fall through to cloud-only for HYBRID mode
        }
    }
#endif

    if (s_current_mode != SMART_VOICE_MODE_LOCAL_ONLY)
    {
        esp_err_t ret = cloud_integration_send_wake_word(SMART_VOICE_WAKE_WORD);
        if (ret == ESP_OK)
        {
            // Transition to LISTENING state - now we send audio to cloud
            s_session_start_tick = xTaskGetTickCount(); // Record session start time
            s_session_ended      = false; // Reset session ended flag for new conversation
            set_wake_state(WAKE_STATE_LISTENING);

            // Reset send timers to prevent immediate timeout
            last_successful_send      = xTaskGetTickCount();
            consecutive_send_failures = 0;

            ESP_LOGI(TAG, "Wake state: IDLE → LISTENING (waiting for user speech)");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to send wake word, staying in IDLE");
            set_wake_state(WAKE_STATE_IDLE);
        }
    }
    else
    {
        // Local only mode - just notify and stay idle for next command
        set_wake_state(WAKE_STATE_IDLE);
    }
}

/**
 * @brief Process wake word detection from audio_processor
 */
static void process_wake_word_detected(void)
{
    ESP_LOGI(TAG, "Wake word detected: '%s'", SMART_VOICE_WAKE_WORD);

    // This function is called from task context when wake word event is received
    // The actual state transition is handled in on_wake_word_detected callback

    // Notify user about wake word (if not already done)
    if (s_user_callback != NULL && get_wake_state() == WAKE_STATE_DETECTED)
    {
        smart_voice_event_t event;
        event.type                     = SMART_VOICE_EVENT_WAKE_WORD;
        event.data.wake_word.wake_word = SMART_VOICE_WAKE_WORD;
        s_user_callback(&event, s_user_data);
    }
}

/**
 * @brief Process voice command recognition result
 */
static void process_command_recognized(const char* command_text)
{
    ESP_LOGI(TAG, "Command recognized: '%s'", command_text);

#if (EVENT_BUS_ENABLE == 1)
    voice_command_data_t voice_data = {
        .command_text = command_text,
        .confidence   = 1.0f,
    };
    event_bus_publish_simple(EVENT_TYPE_VOICE_COMMAND, VOICE_EVENT_COMMAND_RECOGNIZED, &voice_data,
                             sizeof(voice_data));
#endif

    // Try to match with fixed commands first (local priority)
    const smart_voice_command_t* cmd = voice_command_find_by_keyword(command_text);

    if (cmd != NULL)
    {
        ESP_LOGI(TAG, "Matched fixed command: [id=%d] %s", cmd->id, cmd->keyword);

        // Execute fixed command action
        if (s_user_callback != NULL)
        {
            smart_voice_event_t event;
            event.type                   = SMART_VOICE_EVENT_LOCAL_CMD;
            event.data.local_cmd.command = cmd;
            s_user_callback(&event, s_user_data);
        }
        return; // Fixed command handled locally
    }

    // No match in fixed commands, forward to cloud
    if (s_current_mode == SMART_VOICE_MODE_LOCAL_ONLY)
    {
        ESP_LOGW(TAG, "No matching command and cloud unavailable");

        // Notify user that command is not recognized
        if (s_user_callback != NULL)
        {
            smart_voice_event_t event;
            event.type                   = SMART_VOICE_EVENT_CMD_NOT_RECOGNIZED;
            event.data.unrecognized.text = command_text;
            s_user_callback(&event, s_user_data);
        }
    }
    else
    {
        ESP_LOGD(TAG, "Forwarding unrecognized command to cloud");
        // Cloud will handle it via audio stream
    }
}

/**
 * @brief Main task for processing audio and voice events
 */
static void smart_voice_task_entry(void* arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Smart Voice Control Task started");

    while (s_initialized)
    {
        smart_voice_event_t event;

        if (s_pending_cloud_start)
        {
            s_pending_cloud_start = false;
            ESP_LOGI(TAG, "=== Activating cloud session ===");

            // Step 1: Unload MultiNet to free ~58KB memory for TLS handshake
            // This is critical to avoid SPI DMA allocation failures during TLS
#if (SMART_VOICE_ENABLE_MULTINET == 1)
            ESP_LOGI(TAG, "Step 1: Unloading MultiNet to free memory for cloud...");
            esp_err_t unload_ret = multinet_unload();
            if (unload_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "MultiNet unload warning (continuing anyway)");
            }
            // Small delay to allow memory to be fully freed
            vTaskDelay(pdMS_TO_TICKS(100));
#endif

            // Step 2: Ensure Cloud Send Task exists before activating cloud
            ensure_cloud_send_task();

            // Step 3: Activate cloud connection (TLS handshake happens here)
            esp_err_t act_ret = cloud_integration_activate();
            if (act_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to activate cloud: %s", esp_err_to_name(act_ret));
                set_wake_state(WAKE_STATE_IDLE);
                // TODO: Consider reloading MultiNet here on failure?
            }
            else
            {
                ESP_LOGI(TAG, "✅ Cloud session activated successfully");
            }
        }

        // 方案P-L: MultiNet reload is now deferred to next wake word detection
        // (see on_wake_word_detected() for the actual reload logic)
        // We just log that a reload is pending - no action needed here
#if (SMART_VOICE_ENABLE_MULTINET == 1)
        if (s_pending_multinet_reload)
        {
            ESP_LOGI(TAG, "📋 方案P-L: MultiNet reload pending (will trigger on next wake word)");
            // Keep s_pending_multinet_reload = true for on_wake_word_detected() to handle
        }
#endif

        // Handle WebSocket disconnection auto-reconnect
        esp_err_t reconnect_ret = cloud_integration_check_reconnect();
        if (reconnect_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Auto-reconnect issue: %s", esp_err_to_name(reconnect_ret));
            // Note: Don't change state here - let the normal flow handle it
        }

        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            switch (event.type)
            {
            case SMART_VOICE_EVENT_WAKE_WORD:
                process_wake_word_detected();
                break;

            case SMART_VOICE_EVENT_COMMAND_TEXT:
                process_command_recognized(event.data.command_text.text);
                break;

            case SMART_VOICE_EVENT_AUDIO_DATA:
                // Forward audio data to cloud (only in LISTENING/CONVERSING states)
                // CRITICAL: Don't send in WAITING_FOR_SPEECH or IDLE to save memory!
                if (cloud_integration_is_ready())
                {
                    wake_state_t send_state = get_wake_state();
                    if (send_state == WAKE_STATE_LISTENING || send_state == WAKE_STATE_CONVERSING)
                    {
                        cloud_integration_send_audio((const int16_t*)event.data.audio.data,
                                                     event.data.audio.length);
                    }
                    // else: Skip sending to conserve memory during WAITING/IDLE
                }
                break;

            default:
                ESP_LOGW(TAG, "Unhandled event type: %d", event.type);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Smart Voice Control Task exiting");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

// Audio input task configuration
#define AUDIO_INPUT_BUFFER_SIZE 1024 // 1024 bytes = 256 samples (32-bit)
// OPUS_FRAME_SIZE is defined above with the async cloud send queue config

// I2S Reader task configuration (low priority, only reads from I2S)
#define I2S_READER_TASK_STACK_SIZE 4096 // Stack for simple I2S reading
#define I2S_READER_TASK_PRIORITY 5      // Low priority to avoid conflict with TTS playback!
#define I2S_READ_TIMEOUT_MS 100         // 100ms timeout (same as audio_service_module)

// Audio Process task configuration (medium priority, handles wake word & cloud)
#define AUDIO_PROCESS_TASK_STACK_SIZE 8192 // Stack for WakeNet/AFE processing
#define AUDIO_PROCESS_TASK_PRIORITY 10     // Medium priority for audio processing

// PCM queue configuration (I2S reader → Audio process)
#define PCM_QUEUE_LENGTH 8    // Queue depth (balance latency vs memory)
#define PCM_CHUNK_SAMPLES 256 // Samples per queue item (16ms @ 16kHz)

typedef struct
{
    int16_t data[PCM_CHUNK_SAMPLES]; // PCM sample data
    int     length;                  // Number of valid samples (0-PCM_CHUNK_SAMPLES)
} pcm_queue_item_t;

/**
 * @brief I2S Reader Task - LOW PRIORITY task that only reads from I2S RX
 *
 * This task is separated from audio processing to avoid blocking issues
 * during TTS playback. It runs at low priority (5) to avoid conflicts with
 * the I2S TX (TTS playback) task.
 *
 * Data flow: I2S RX → PCM Queue → Audio Process Task
 */
static void i2s_reader_task(void* arg)
{
    (void)arg;

    ESP_LOGI(TAG, "I2S Reader Task started (priority=%d, timeout=%dms)", I2S_READER_TASK_PRIORITY,
             I2S_READ_TIMEOUT_MS);

    i2s_chan_handle_t rx_handle = audio_i2s_get_rx_handle();
    if (rx_handle == NULL)
    {
        ESP_LOGE(TAG, "I2S RX handle not available, reader task exiting");
        s_i2s_reader_running     = false;
        s_i2s_reader_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "I2S Reader: RX handle obtained");

    uint8_t* i2s_buffer = (uint8_t*)malloc(AUDIO_INPUT_BUFFER_SIZE);
    if (i2s_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate I2S buffer");
        s_i2s_reader_running     = false;
        s_i2s_reader_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t read_count       = 0;
    uint32_t timeout_count    = 0;
    uint32_t queue_full_count = 0;

    vTaskDelay(pdMS_TO_TICKS(200)); // Wait for I2S to stabilize

    ESP_LOGI(TAG, "I2S Reader: entering main loop");

    while (s_i2s_reader_running && s_initialized)
    {
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(
            rx_handle, i2s_buffer, AUDIO_INPUT_BUFFER_SIZE, &bytes_read,
            pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS) // 100ms timeout - same as audio_service_module!
        );

        read_count++;

        if (ret == ESP_OK && bytes_read > 0)
        {
            timeout_count = 0;

            int32_t* i2s_samples = (int32_t*)i2s_buffer;
            int      samples     = bytes_read / sizeof(int32_t);

            // Convert to 16-bit PCM and send to queue in chunks
            pcm_queue_item_t queue_item;
            int              item_index = 0;

            for (int i = 0; i < samples; i++)
            {
                int32_t sample = i2s_samples[i] >> 16;
                if (sample > INT16_MAX)
                    sample = INT16_MAX;
                if (sample < INT16_MIN)
                    sample = INT16_MIN;
                queue_item.data[item_index++] = (int16_t)sample;

                // When chunk is full, send to queue
                if (item_index >= PCM_CHUNK_SAMPLES)
                {
                    queue_item.length = item_index;

                    // Non-blocking send - drop data if queue is full (avoid blocking!)
                    if (xQueueSend(s_pcm_queue, &queue_item, 0) != pdTRUE)
                    {
                        queue_full_count++;
                        if (queue_full_count % 100 == 1)
                        {
                            ESP_LOGW(TAG, "I2S Reader: PCM queue full (dropped %u chunks)",
                                     queue_full_count);
                        }
                    }
                    item_index = 0;
                }
            }

            // Send remaining samples if any
            if (item_index > 0)
            {
                queue_item.length = item_index;
                if (xQueueSend(s_pcm_queue, &queue_item, 0) != pdTRUE)
                {
                    queue_full_count++;
                }
            }
        }
        else if (ret == ESP_ERR_TIMEOUT)
        {
            timeout_count++;
            if (timeout_count <= 3 || timeout_count % 50 == 0)
            {
                ESP_LOGD(TAG, "I2S Reader: timeout #%u", timeout_count);
            }
        }
        else
        {
            timeout_count++;
            if (timeout_count % 100 == 1)
            {
                ESP_LOGW(TAG, "I2S Reader: error=%s (#%u)", esp_err_to_name(ret), timeout_count);
            }
        }

        // Periodic stats log every ~30 seconds
        if (read_count % 300 == 0)
        {
            ESP_LOGI(TAG, "I2S Reader: reads=%u, timeouts=%u, queue_overflows=%u", read_count,
                     timeout_count, queue_full_count);
        }
    }

    free(i2s_buffer);
    ESP_LOGI(TAG, "I2S Reader Task stopped (reads=%u)", read_count);
    s_i2s_reader_task_handle = NULL;
    s_i2s_reader_running     = false;
    vTaskDelete(NULL);
}

/**
 * @brief Audio Process Task - handles wake word detection and cloud audio sending
 *
 * This task receives PCM data from the I2S Reader via a FreeRTOS queue.
 * It performs WakeNet detection and sends audio to the cloud when in active session.
 * Runs at medium priority (10), independent of I2S reading.
 *
 * Data flow: PCM Queue → WakeNet/AFE → Cloud (Opus)
 */

/**
 * @brief Cloud send task - handles async audio sending to cloud
 *
 * This task runs independently to prevent the audio process task from
 * blocking on WebSocket sends. It reads from the cloud send queue and
 * sends data to the cloud service.
 */
static void cloud_send_task(void* arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Cloud Send Task started");

    uint32_t   send_success_count = 0;
    uint32_t   send_fail_count    = 0;
    TickType_t last_log_tick      = xTaskGetTickCount();

    while (s_cloud_send_task_running)
    {
        cloud_send_item_t send_item;

        // Wait for data with timeout (allows checking running flag)
        if (xQueueReceive(s_cloud_send_queue, &send_item, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            // No data - check if we should log status
            TickType_t now = xTaskGetTickCount();
            if (now - last_log_tick > pdMS_TO_TICKS(30000))
            {
                last_log_tick = now;
                ESP_LOGI(TAG, "Cloud Send Task: success=%lu, fail=%lu",
                         (unsigned long)send_success_count, (unsigned long)send_fail_count);
            }
            continue;
        }

        // Send to cloud (this may block, but it's OK since we're in a dedicated task)
        // Note: pcm_length is sample count, cloud_integration expects bytes
        // CRITICAL: Check state before sending to avoid memory waste in WAITING/IDLE
        wake_state_t task_send_state = get_wake_state();
        esp_err_t    ret             = ESP_OK;

        if (task_send_state == WAKE_STATE_LISTENING || task_send_state == WAKE_STATE_CONVERSING)
        {
            ret = cloud_integration_send_audio(send_item.pcm_data,
                                               send_item.pcm_length * sizeof(int16_t));
        }
        else
        {
            // State changed to WAITING or IDLE - skip sending to save memory
            ESP_LOGD(TAG, "Skipping cloud send (state=%d, not LISTENING/CONVERSING)",
                     task_send_state);
            ret = ESP_OK; // Not an error, just skipped
        }

        // Free the dynamically allocated PCM buffer and decrement counter
        free(send_item.pcm_data);
        send_item.pcm_data = NULL;
        s_active_allocations--; // Decrement allocation counter

        // Note: No vTaskDelay here - this is a dedicated task, no TW risk
        // Yielding would slow down processing and cause queue overflow

        if (ret == ESP_OK)
        {
            send_success_count++;
        }
        else
        {
            send_fail_count++;
            ESP_LOGW(TAG, "Cloud send failed: %s (total fails: %lu)", esp_err_to_name(ret),
                     (unsigned long)send_fail_count);

            // If too many failures, wait a bit before retrying
            if (send_fail_count % 10 == 0)
            {
                ESP_LOGW(TAG, "Multiple send failures, waiting 500ms...");
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }

    ESP_LOGI(TAG, "Cloud Send Task stopped (success=%lu, fail=%lu)",
             (unsigned long)send_success_count, (unsigned long)send_fail_count);
    s_cloud_send_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Ensure Cloud Send Task is created (lazy initialization)
 *
 * Creates the Cloud Send Task and its queue on first call.
 * Subsequent calls are no-ops if already created.
 * This defers ~4KB memory allocation until cloud session is actually needed.
 */
// 方案Q: PSRAM task stacks (save Internal RAM for SDIO DMA)
static StackType_t* s_cloud_send_task_stack = NULL;
#define CLOUD_SEND_TASK_STACK_SIZE 4096
static StackType_t* s_audio_process_task_stack = NULL;
static StaticTask_t s_cloud_send_task_buffer;
static StaticTask_t s_audio_process_task_buffer;

static esp_err_t ensure_cloud_send_task(void)
{
    if (s_cloud_send_task_handle != NULL && s_cloud_send_task_running)
    {
        return ESP_OK; // Already created
    }

    ESP_LOGI(TAG, "Creating Cloud Send Task on-demand (PSRAM stack)...");

    s_cloud_send_queue = xQueueCreate(CLOUD_SEND_QUEUE_LENGTH, sizeof(cloud_send_item_t));
    if (s_cloud_send_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create cloud send queue");
        return ESP_ERR_NO_MEM;
    }

    // 方案Q: Allocate task stack in PSRAM to save Internal RAM (~4KB)
    if (s_cloud_send_task_stack == NULL)
    {
        s_cloud_send_task_stack = (StackType_t*)heap_caps_malloc(
            CLOUD_SEND_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        if (s_cloud_send_task_stack == NULL)
        {
            ESP_LOGW(TAG, "[PSRAM] Failed to allocate Cloud Send Task stack in PSRAM, fallback to "
                          "Internal RAM");
            s_cloud_send_task_stack =
                (StackType_t*)malloc(CLOUD_SEND_TASK_STACK_SIZE * sizeof(StackType_t));
        }
        else
        {
            ESP_LOGI(TAG, "[PSRAM] ✅ Cloud Send Task stack allocated in PSRAM (%d bytes)",
                     CLOUD_SEND_TASK_STACK_SIZE * sizeof(StackType_t));
        }
    }

    if (s_cloud_send_task_stack == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate Cloud Send Task stack");
        vQueueDelete(s_cloud_send_queue);
        s_cloud_send_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cloud_send_task_running = true;

    // Use xTaskCreateStatic with PSRAM stack to avoid Internal RAM usage
    // ESP-IDF FreeRTOS uses 7-parameter API (includes pvParameters at position 4)
    TaskHandle_t new_handle =
        xTaskCreateStatic(cloud_send_task,            // 1: Task function
                          "cloud_send",               // 2: Task name
                          CLOUD_SEND_TASK_STACK_SIZE, // 3: Stack depth (words)
                          NULL,                       // 4: Task parameters (none needed)
                          8,                          // 5: Priority
                          s_cloud_send_task_stack,    // 6: Stack buffer (PSRAM)
                          &s_cloud_send_task_buffer   // 7: Task control block buffer
        );

    if (new_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create cloud send task (static)");
        free(s_cloud_send_task_stack);
        s_cloud_send_task_stack   = NULL;
        s_cloud_send_task_running = false;
        vQueueDelete(s_cloud_send_queue);
        s_cloud_send_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cloud_send_task_handle = new_handle;

    // Reset allocation counter (no active allocations at startup)
    s_active_allocations = 0;

    ESP_LOGI(TAG, "✅ Cloud Send Task created (PSRAM stack, priority=8, max_allocs=%d)",
             MAX_ACTIVE_ALLOCATIONS);
    return ESP_OK;
}

static void audio_process_task(void* arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Audio Process Task started (priority=%d)", AUDIO_PROCESS_TASK_PRIORITY);
    ESP_LOGI(TAG, "Wake state machine initialized: IDLE (waiting for wake word)");

    int16_t* pcm_accum_buffer = (int16_t*)malloc(OPUS_FRAME_SIZE * sizeof(int16_t));

    if (pcm_accum_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate accumulation buffer");
        s_audio_process_task_running = false;
        s_audio_process_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (s_audio_processor != NULL && s_feed_chunksize > 0)
    {
        s_wakenet_buffer = (int16_t*)malloc(s_feed_chunksize * sizeof(int16_t));
        if (s_wakenet_buffer == NULL)
        {
            ESP_LOGW(TAG, "Failed to allocate wakenet buffer, wake word disabled");
        }

#if (SMART_VOICE_ENABLE_MULTINET == 1)
        s_multinet_buffer = (int16_t*)malloc(MULTINET_BUFFER_SIZE * sizeof(int16_t));
        if (s_multinet_buffer == NULL)
        {
            ESP_LOGW(TAG, "Failed to allocate multinet buffer, local command recognition disabled");
        }
#endif
    }

    uint32_t process_count      = 0;
    uint32_t cloud_send_count   = 0;
    uint32_t wakenet_feed_count = 0;
    uint32_t last_log_tick      = 0;
    int      pcm_accum_index    = 0;
    int      wakenet_index      = 0;

    ESP_LOGI(TAG, "Audio Process Task: entering main loop (waiting for PCM data...)");

    uint32_t consecutive_queue_fulls __attribute__((unused)) =
        0;                                                    // Track consecutive queue full events
    TickType_t last_successful_process = xTaskGetTickCount(); // Last time we processed data
    TickType_t loop_start_tick         = 0;                   // For intelligent CPU yielding
#define CPU_YIELD_THRESHOLD_MS 50                             // Only yield if processing took >50ms
    // Note: consecutive_send_failures and last_successful_send are now global variables

#define MAX_CONSECUTIVE_SEND_FAILURES 10 // Max failures before reset
#define SEND_FAILURE_RESET_MS \
    10000 // No successful send for this long → reset (increased from 3000)

    while (s_audio_process_task_running && s_initialized)
    {
        pcm_queue_item_t queue_item;

        if (xQueueReceive(s_pcm_queue, &queue_item, pdMS_TO_TICKS(1000)) != pdTRUE)
        {
            // No data within 1s - check if we're stuck
            TickType_t now                     = xTaskGetTickCount();
            TickType_t time_since_last_process = now - last_successful_process;

            // If no data for >5 seconds and I2S is running, something is wrong
            if (time_since_last_process > pdMS_TO_TICKS(5000) && s_i2s_reader_running)
            {
                ESP_LOGW(TAG, "Audio Process: No data for %ldms, checking system health...",
                         (long)(time_since_last_process * portTICK_PERIOD_MS));

                // Force reset AFE if it seems stuck
#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                if (s_audio_processor != NULL)
                {
                    ESP_LOGW(TAG, "Audio Process: Force resetting AFE due to inactivity");
                    audio_processor_reset(s_audio_processor);
                    wakenet_index = 0; // Reset buffer index
                }
#endif

                // Reset state to IDLE if stuck in non-IDLE state
                wake_state_t stuck_state = get_wake_state();
                if (stuck_state != WAKE_STATE_IDLE)
                {
                    ESP_LOGW(TAG, "Audio Process: Force reset wake state %d → IDLE (stuck)",
                             stuck_state);
                    set_wake_state(WAKE_STATE_IDLE);
                    s_session_start_tick = 0;
                }

                last_successful_process = now; // Update to avoid repeated resets
            }
            continue; // Check running flag and loop
        }

        // Successfully received data
        last_successful_process = xTaskGetTickCount();
        consecutive_queue_fulls = 0; // Reset counter
        process_count++;
        loop_start_tick =
            xTaskGetTickCount(); // Track processing start time for intelligent yielding

        for (int i = 0; i < queue_item.length; i++)
        {
            int16_t pcm_sample = queue_item.data[i];

            // Feed to WakeNet if enabled (all states - required for AFE ringbuffer health)
#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
            if (s_wakenet_buffer != NULL && s_audio_processor != NULL)
            {
                s_wakenet_buffer[wakenet_index++] = pcm_sample;

                if (wakenet_index >= s_feed_chunksize)
                {
                    audio_processor_feed(s_audio_processor, s_wakenet_buffer, s_feed_chunksize,
                                         NULL, 0);
                    wakenet_index = 0;
                    wakenet_feed_count++;
                }
            }
#endif

#if (SMART_VOICE_ENABLE_MULTINET == 1)
            if (s_multinet_buffer != NULL)
            {
                s_multinet_buffer[s_multinet_index++] = pcm_sample;
                if (s_multinet_index >= MULTINET_BUFFER_SIZE)
                {
                    wake_state_t local_state = get_wake_state();
                    if (local_state == WAKE_STATE_LOCAL_RECOGNIZING && s_multinet != NULL)
                    {
                        esp_mn_state_t mn_state = s_multinet->detect(s_mn_data, s_multinet_buffer);
                        // Note: No vTaskDelay(1) here - let the intelligent CPU yield at loop end
                        // handle it
                        if (mn_state == ESP_MN_STATE_DETECTED)
                        {
                            esp_mn_results_t* mn_res = s_multinet->get_results(s_mn_data);
                            for (int m = 0; m < mn_res->num; m++)
                            {
                                int cmd_id = mn_res->command_id[m];
                                ESP_LOGI(TAG,
                                         "MultiNet detected: cmd_id=%d keyword='%s' confidence=%d",
                                         cmd_id, get_local_cmd_keyword(cmd_id), mn_res->prob[m]);
                                s_multinet_speech_detected = true;

                                execute_local_command(cmd_id);
                                if (cmd_id == SMART_VOICE_LOCAL_CMD_CLOUD_ON)
                                {
                                    ESP_LOGI(TAG, "CLOUD_ON command handled, waiting for main task "
                                                  "to start cloud");
                                }
                                else
                                {
                                    s_local_recog_start_tick   = xTaskGetTickCount();
                                    s_multinet_speech_detected = false;
                                    ESP_LOGI(
                                        TAG,
                                        "Local command executed, recognition window reset (15s)");
#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                                    if (s_audio_processor != NULL)
                                    {
                                        audio_processor_reset(s_audio_processor);
                                        wakenet_index = 0;
                                    }
#endif
                                }
                            }
                        }
                    }
                    s_multinet_index = 0;
                }
            }
#endif

            wake_state_t send_state = get_wake_state();
            if ((send_state == WAKE_STATE_LISTENING || send_state == WAKE_STATE_CONVERSING) &&
                cloud_integration_is_ready())
            {
                pcm_accum_buffer[pcm_accum_index++] = pcm_sample;

                if (pcm_accum_index >= OPUS_FRAME_SIZE)
                {
                    // Async send: ensure task exists, then queue data
                    if (s_cloud_send_queue == NULL)
                    {
                        ensure_cloud_send_task(); // Lazy creation
                    }

                    if (s_cloud_send_queue != NULL)
                    {
                        cloud_send_item_t send_item;

                        // Check allocation limit to prevent memory exhaustion
                        if (s_active_allocations >= MAX_ACTIVE_ALLOCATIONS)
                        {
                            // Too many active allocations - wait for consumer to free some
                            consecutive_send_failures++;
                            ESP_LOGW(TAG, "Allocation limit reached (%d/%d), skipping",
                                     s_active_allocations, MAX_ACTIVE_ALLOCATIONS);
                            pcm_accum_index = 0;
                            continue;
                        }

                        // Allocate memory for PCM data in SPIRAM (saves internal RAM)
                        send_item.pcm_data = (int16_t*)heap_caps_malloc(
                            OPUS_FRAME_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                        if (send_item.pcm_data == NULL)
                        {
                            // Fallback to internal RAM if SPIRAM allocation fails
                            send_item.pcm_data =
                                (int16_t*)malloc(OPUS_FRAME_SIZE * sizeof(int16_t));
                        }
                        if (send_item.pcm_data == NULL)
                        {
                            // Memory allocation failed - count as failure
                            consecutive_send_failures++;
                            ESP_LOGW(
                                TAG,
                                "Failed to allocate PCM buffer for queue (consecutive: %lu/%lu)",
                                consecutive_send_failures,
                                (unsigned long)MAX_CONSECUTIVE_SEND_FAILURES);
                            pcm_accum_index = 0;
                            continue;
                        }

                        // Increment allocation counter
                        s_active_allocations++;

                        memcpy(send_item.pcm_data, pcm_accum_buffer,
                               OPUS_FRAME_SIZE * sizeof(int16_t));
                        send_item.pcm_length = OPUS_FRAME_SIZE; // Store as sample count!

                        if (xQueueSend(s_cloud_send_queue, &send_item, 0) != pdTRUE)
                        {
                            // Queue full - free memory and count as failure
                            free(send_item.pcm_data);
                            send_item.pcm_data = NULL;
                            s_active_allocations--; // Decrement since not queued
                            consecutive_send_failures++;
                            ESP_LOGW(TAG, "Cloud send queue full (consecutive: %lu/%lu)",
                                     consecutive_send_failures,
                                     (unsigned long)MAX_CONSECUTIVE_SEND_FAILURES);

                            if (consecutive_send_failures >= MAX_CONSECUTIVE_SEND_FAILURES)
                            {
                                ESP_LOGE(TAG,
                                         "Send queue full too often, forcing state reset to IDLE");
                                set_wake_state(WAKE_STATE_IDLE);
                                s_session_start_tick      = 0;
                                s_waiting_start_tick      = 0;
                                consecutive_send_failures = 0;
                            }
                        }
                        else
                        {
                            // Successfully queued
                            cloud_send_count++;
                            consecutive_send_failures = 0;
                            last_successful_send      = xTaskGetTickCount();
                        }
                    }
                    pcm_accum_index = 0;
                }
            }
            else
            {
                pcm_accum_index = 0;
            }
        }

        // Intelligent CPU yielding: only yield if this iteration took too long
        // This balances TW prevention with audio pipeline throughput
        TickType_t loop_end_tick    = xTaskGetTickCount();
        TickType_t loop_duration_ms = (loop_end_tick - loop_start_tick) * portTICK_PERIOD_MS;
        if (loop_duration_ms > CPU_YIELD_THRESHOLD_MS)
        {
            vTaskDelay(1);
        }

        TickType_t now = xTaskGetTickCount();

        wake_state_t current_state = get_wake_state();
        if (current_state != WAKE_STATE_IDLE && s_session_start_tick > 0)
        {
            TickType_t elapsed = now - s_session_start_tick;
            if (elapsed > pdMS_TO_TICKS(SESSION_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "Session timeout! State=%d elapsed=%ldms, force reset to IDLE",
                         current_state, (long)(elapsed * portTICK_PERIOD_MS));

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                if (s_audio_processor != NULL)
                {
                    audio_processor_reset(s_audio_processor);
                }
#endif

                // Trigger MultiNet reload if we were in cloud mode (MultiNet was unloaded)
#if (SMART_VOICE_ENABLE_MULTINET == 1)
                if (current_state == WAKE_STATE_LISTENING ||
                    current_state == WAKE_STATE_CONVERSING ||
                    current_state == WAKE_STATE_WAITING_FOR_SPEECH)
                {
                    s_pending_multinet_reload = true;
                    ESP_LOGI(TAG, "Session timeout in cloud mode - scheduling MultiNet reload");
                }
#endif

                set_wake_state(WAKE_STATE_IDLE);
                s_session_start_tick = 0;
            }
        }

        // Check silence timeout in WAITING_FOR_SPEECH state
        if (current_state == WAKE_STATE_WAITING_FOR_SPEECH && s_waiting_start_tick > 0)
        {
            TickType_t waiting_elapsed = now - s_waiting_start_tick;
            if (waiting_elapsed > pdMS_TO_TICKS(SILENCE_TIMEOUT_MS))
            {
                // No speech detected within timeout period - go back to IDLE
                ESP_LOGI(TAG, "Silence timeout in WAITING state (%ldms), no speech detected → IDLE",
                         (long)(waiting_elapsed * portTICK_PERIOD_MS));

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                if (s_audio_processor != NULL)
                {
                    audio_processor_reset(s_audio_processor);
                }
#endif

                // Trigger MultiNet reload if in cloud mode
#if (SMART_VOICE_ENABLE_MULTINET == 1)
                s_pending_multinet_reload = true;
                ESP_LOGI(TAG, "Silence timeout in cloud mode - scheduling MultiNet reload");
#endif

                set_wake_state(WAKE_STATE_IDLE);
                s_session_start_tick = 0;
                s_waiting_start_tick = 0;
            }
        }

#if (SMART_VOICE_ENABLE_MULTINET == 1)
        if (current_state == WAKE_STATE_LOCAL_RECOGNIZING && s_local_recog_start_tick > 0)
        {
            TickType_t local_elapsed = now - s_local_recog_start_tick;
            if (local_elapsed > pdMS_TO_TICKS(15000))
            {
                ESP_LOGI(TAG, "Local recognition window expired (%ldms) → IDLE",
                         (long)(local_elapsed * portTICK_PERIOD_MS));
                set_wake_state(WAKE_STATE_IDLE);
                s_local_recog_start_tick   = 0;
                s_multinet_speech_detected = false;

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
                if (s_audio_processor != NULL)
                {
                    audio_processor_reset(s_audio_processor);
                    wakenet_index = 0;
                }
#endif
            }
        }
#endif

        // Check for send timeout in LISTENING/CONVERSING state
        if ((current_state == WAKE_STATE_LISTENING || current_state == WAKE_STATE_CONVERSING) &&
            last_successful_send > 0)
        {
            TickType_t send_elapsed = now - last_successful_send;
            if (send_elapsed > pdMS_TO_TICKS(SEND_FAILURE_RESET_MS))
            {
                ESP_LOGW(TAG,
                         "No successful audio send for %ldms (state=%d), possible deadlock → IDLE",
                         (long)(send_elapsed * portTICK_PERIOD_MS), current_state);
                set_wake_state(WAKE_STATE_IDLE);
                s_session_start_tick      = 0;
                s_waiting_start_tick      = 0;
                consecutive_send_failures = 0;
                last_successful_send      = now; // Reset to avoid repeated triggers
            }
        }

        if (now - last_log_tick > pdMS_TO_TICKS(10000))
        {
            last_log_tick          = now;
            wake_state_t log_state = get_wake_state();
            ESP_LOGI(TAG, "Audio Process: processed=%u, feeds=%u, sends=%u, state=%d",
                     process_count, wakenet_feed_count, cloud_send_count, log_state);
        }
    }

    free(pcm_accum_buffer);
    if (s_wakenet_buffer != NULL)
    {
        free(s_wakenet_buffer);
        s_wakenet_buffer = NULL;
    }

#if (SMART_VOICE_ENABLE_MULTINET == 1)
    if (s_multinet_buffer != NULL)
    {
        free(s_multinet_buffer);
        s_multinet_buffer = NULL;
    }
#endif

    ESP_LOGI(TAG, "Audio Process Task stopped (processed=%u, cloud_sends=%u)", process_count,
             cloud_send_count);
    s_audio_process_task_handle  = NULL;
    s_audio_process_task_running = false;
    vTaskDelete(NULL);
}

/**
 * @brief Initialize the Smart Voice Control Module
 */
esp_err_t smart_voice_control_module_init(const smart_voice_config_params_t* config)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Module already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    // Create mutex
    if (s_state_mutex == NULL)
    {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create state mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Store configuration
    if (config != NULL)
    {
        memcpy(&s_config, config, sizeof(smart_voice_config_params_t));
    }
    else
    {
        // Use default configuration
        memset(&s_config, 0, sizeof(smart_voice_config_params_t));
        s_config.mode = SMART_VOICE_MODE_HYBRID;
    }

    // Initialize sub-modules

    // 1. Voice Command Parser (JSON config loading)
    ret = voice_command_parser_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize voice command parser: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // 2. Network Monitor (WiFi status tracking)
    ret = network_monitor_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize network monitor: %s", esp_err_to_name(ret));
        goto cleanup_parser;
    }

    // Set network callback for auto-fallback
    network_monitor_set_callback(on_network_status_changed, NULL);

    // 3. Cloud Integration (xiaozhi chat)
    ret = cloud_integration_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize cloud integration: %s", esp_err_to_name(ret));
        // Non-fatal, can work in local-only mode
        ESP_LOGW(TAG, "Continuing in LOCAL_ONLY mode");
        s_current_mode = SMART_VOICE_MODE_LOCAL_ONLY;
    }

    // Set cloud callback for event forwarding
    cloud_integration_set_callback(on_cloud_event, NULL);

    // 4. Audio Processor (WakeNet - local wake word detection)
    // NOTE: WakeNet (audio_processor) may crash on some hardware configurations
    // If SMART_VOICE_ENABLE_WAKE_WORD is disabled, we skip initialization entirely

    s_audio_processor = NULL;
    s_feed_chunksize  = 0;

#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
    ESP_LOGI(TAG, "Initializing audio processor for wake word detection...");

    audio_processor_config_t proc_config = {
        .enable_aec           = false, // Disabled - no reference channel available
        .enable_vad           = true,  // VAD for voice activity detection
        .enable_wake_word     = true,
        .wake_word_model_name = SMART_VOICE_WAKE_WORD_MODEL,
        .mic_channels         = 1,
        .ref_channels         = 0, // No reference channel
        .sample_rate          = 16000,
    };

    ESP_LOGW(TAG, "Attempting to create audio processor (this may crash on some configs)...");
    s_audio_processor = audio_processor_create(&proc_config);

    if (s_audio_processor != NULL)
    {
        audio_processor_set_wake_word_callback(s_audio_processor, on_wake_word_detected, NULL);
        audio_processor_set_vad_callback(s_audio_processor, on_vad_state_changed,
                                         NULL); // Set VAD callback
        s_feed_chunksize = audio_processor_get_feed_chunksize(s_audio_processor);
        ESP_LOGI(TAG, "Audio processor created (feed_chunksize=%d)", s_feed_chunksize);

        // NOTE: audio_processor_start() is called in smart_voice_control_module_start()
        // AFTER I2S is initialized to avoid "AFE Ringbuffer empty" warnings
        ESP_LOGI(TAG, "✅ Audio processor created (wake word: %s) - will start after I2S init",
                 SMART_VOICE_WAKE_WORD_MODEL);
    }
    else
    {
        ESP_LOGW(TAG, "⚠️ Failed to create audio processor");
    }
#endif // SMART_VOICE_ENABLE_WAKE_WORD

    if (s_audio_processor == NULL)
    {
        ESP_LOGW(TAG, "Wake word detection not available");
        ESP_LOGW(TAG, "System will work in continuous cloud-listening mode");
        set_wake_state(WAKE_STATE_LISTENING); // Default to listening mode
        ESP_LOGI(TAG, "Default mode: LISTENING (audio sent to cloud continuously)");
    }

#if (SMART_VOICE_ENABLE_MULTINET == 1)
    ret = multinet_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "MultiNet init failed (%s), local command recognition disabled",
                 esp_err_to_name(ret));
    }
#endif

    // Create event queue
    s_event_queue = xQueueCreate(10, sizeof(smart_voice_event_t));
    if (s_event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event queue");
        goto cleanup_cloud;
    }

    // Set initial mode based on configuration
    s_current_mode = s_config.mode;

    if (s_current_mode == SMART_VOICE_MODE_CLOUD_ONLY && network_monitor_is_connected())
    {
        ret = cloud_integration_start();
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to start cloud session: %s", esp_err_to_name(ret));
            s_current_mode = SMART_VOICE_MODE_LOCAL_ONLY;
        }
    }
    else if (!network_monitor_is_connected() && s_current_mode == SMART_VOICE_MODE_CLOUD_ONLY)
    {
        ESP_LOGW(TAG, "Network not available for CLOUD_ONLY mode, using LOCAL_ONLY");
        s_current_mode = SMART_VOICE_MODE_LOCAL_ONLY;
    }
    else if (s_current_mode == SMART_VOICE_MODE_HYBRID)
    {
        ESP_LOGI(TAG,
                 "HYBRID mode: cloud will be started on-demand (local commands or '开启聊天')");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Smart Voice Control Module initialized (mode=%d)", s_current_mode);
    return ESP_OK;

cleanup_cloud:
    cloud_integration_deinit();
cleanup_parser:
    voice_command_parser_deinit();
cleanup:
    vSemaphoreDelete(s_state_mutex);
    s_state_mutex = NULL;
    return ret;
}

/**
 * @brief Start the Smart Voice Control Module
 */
esp_err_t smart_voice_control_module_start(void)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Task already running");
        return ESP_OK;
    }

    // === Phase 1: Cloud Connection Strategy ===
    // DISABLED: Pre-connect to save ~30KB memory during startup
    // Original: cloud_integration_preconnect() was called here to establish TLS/WebSocket early
    // New strategy: Delay connection until user says "开启聊天" (on-demand activation)
    // Benefit: Frees up 30KB+ memory for offline voice recognition (AFE/WakeNet/MultiNet)
    smart_voice_mode_t current_mode = smart_voice_get_mode();
    if ((current_mode == SMART_VOICE_MODE_HYBRID || current_mode == SMART_VOICE_MODE_CLOUD_ONLY) &&
        network_monitor_is_connected())
    {
        ESP_LOGI(TAG, "Phase 1: Cloud connection deferred (will connect on '开启聊天' command)");
        ESP_LOGI(TAG, "   Strategy: Save ~30KB memory for offline voice recognition");
    }
    else if (!network_monitor_is_connected())
    {
        ESP_LOGI(TAG, "Phase 1: Skipping cloud (no network)");
    }

    // Initialize audio driver (I2S hardware)
    extern esp_err_t audio_driver_init(const audio_driver_config_t* config);

// Use default configuration from Kconfig
#include "audio_driver_config.h"
    static const audio_driver_config_t s_audio_config = {
        .sample_rate            = AUDIO_DRIVER_SAMPLE_RATE,
        .i2s_num                = AUDIO_DRIVER_I2S_NUM,
        .bclk_gpio              = AUDIO_DRIVER_BCLK_GPIO,
        .ws_gpio                = AUDIO_DRIVER_WS_GPIO,
        .dout_gpio              = AUDIO_DRIVER_DOUT_GPIO,
        .din_gpio               = AUDIO_DRIVER_DIN_GPIO,
        .volume                 = AUDIO_DRIVER_DEFAULT_VOLUME,
        .gain                   = AUDIO_DRIVER_DEFAULT_GAIN,
        .enable_noise_reduction = AUDIO_DRIVER_ENABLE_NOISE_REDUCTION,
        .noise_threshold        = AUDIO_DRIVER_NOISE_THRESHOLD,
    };

    esp_err_t ret = audio_driver_init(&s_audio_config);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to initialize audio driver: %s", esp_err_to_name(ret));
        // Continue anyway - speaker won't work but other features will
    }
    else
    {
        ESP_LOGI(TAG, "Audio driver initialized successfully");
    }

    // Start I2S for audio output (required for speaker playback)
    extern esp_err_t audio_i2s_start(void);
    esp_err_t        i2s_ret = audio_i2s_start();
    if (i2s_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to start I2S: %s", esp_err_to_name(i2s_ret));
    }
    else
    {
        ESP_LOGI(TAG, "I2S started successfully");
    }

    // Start audio processor (AFTER I2S is ready to avoid "AFE Ringbuffer empty")
#if (SMART_VOICE_ENABLE_WAKE_WORD == 1)
    if (s_audio_processor != NULL && !audio_processor_is_running(s_audio_processor))
    {
        esp_err_t proc_ret = audio_processor_start(s_audio_processor);
        if (proc_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to start audio processor: %s", esp_err_to_name(proc_ret));
        }
        else
        {
            ESP_LOGI(TAG, "✅ Audio processor started (I2S ready)");
        }
    }
#endif

    // For CLOUD_ONLY mode: activate cloud session now (preconnect already done in Phase 1)
    smart_voice_mode_t start_mode = smart_voice_get_mode();
    if (start_mode == SMART_VOICE_MODE_CLOUD_ONLY && cloud_integration_is_ready())
    {
        smart_voice_cloud_state_t cstate = cloud_integration_get_state();
        if (cstate == SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE)
        {
            ESP_LOGI(TAG, "CLOUD_ONLY mode: activating preconnected cloud...");
            esp_err_t act_ret = cloud_integration_activate();
            if (act_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to activate cloud: %s", esp_err_to_name(act_ret));
            }
        }
    }
    else if (start_mode == SMART_VOICE_MODE_HYBRID && cloud_integration_is_ready())
    {
        ESP_LOGI(TAG, "HYBRID mode: cloud preconnected, will activate on '开启聊天' command");
    }

    // Create PCM queue for I2S Reader → Audio Process communication
    s_pcm_queue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_queue_item_t));
    if (s_pcm_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create PCM queue");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PCM queue created (length=%d, item_size=%u bytes)", PCM_QUEUE_LENGTH,
             sizeof(pcm_queue_item_t));

    // Create main processing task
    BaseType_t task_ret =
        xTaskCreate(smart_voice_task_entry, "smart_voice_task", SMART_VOICE_TASK_STACK_SIZE, NULL,
                    SMART_VOICE_TASK_PRIORITY, &s_task_handle);

    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create main task");
        vQueueDelete(s_pcm_queue);
        s_pcm_queue = NULL;
        return ESP_FAIL;
    }

    // Create I2S Reader Task (LOW priority - only reads from I2S)
    s_i2s_reader_running = true;
    task_ret = xTaskCreate(i2s_reader_task, "i2s_reader", I2S_READER_TASK_STACK_SIZE, NULL,
                           I2S_READER_TASK_PRIORITY, &s_i2s_reader_task_handle);

    if (task_ret != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to create I2S reader task - microphone input disabled");
        s_i2s_reader_task_handle = NULL;
        s_i2s_reader_running     = false;
    }
    else
    {
        ESP_LOGI(TAG, "I2S Reader Task created (priority=%d)", I2S_READER_TASK_PRIORITY);
    }

    // Create Audio Process Task (MEDIUM priority - handles wake word & cloud)
    // 方案Q: Use PSRAM stack to save ~8KB Internal RAM for SDIO DMA
    s_audio_process_task_running = true;

    if (s_audio_process_task_stack == NULL)
    {
        s_audio_process_task_stack = (StackType_t*)heap_caps_malloc(
            AUDIO_PROCESS_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        if (s_audio_process_task_stack == NULL)
        {
            ESP_LOGW(TAG, "[PSRAM] Failed to allocate Audio Process Task stack in PSRAM, fallback "
                          "to Internal RAM");
            s_audio_process_task_stack =
                (StackType_t*)malloc(AUDIO_PROCESS_TASK_STACK_SIZE * sizeof(StackType_t));
        }
        else
        {
            ESP_LOGI(TAG, "[PSRAM] ✅ Audio Process Task stack allocated in PSRAM (%d bytes)",
                     AUDIO_PROCESS_TASK_STACK_SIZE * sizeof(StackType_t));
        }
    }

    if (s_audio_process_task_stack != NULL)
    {
        // Use xTaskCreateStatic with PSRAM stack (ESP-IDF 7-parameter API)
        TaskHandle_t audio_handle =
            xTaskCreateStatic(audio_process_task,            // 1: Task function
                              "audio_process",               // 2: Task name
                              AUDIO_PROCESS_TASK_STACK_SIZE, // 3: Stack depth (words)
                              NULL,                          // 4: Task parameters (none needed)
                              AUDIO_PROCESS_TASK_PRIORITY,   // 5: Priority
                              s_audio_process_task_stack,    // 6: Stack buffer (PSRAM)
                              &s_audio_process_task_buffer   // 7: Task control block buffer
            );

        if (audio_handle != NULL)
        {
            s_audio_process_task_handle = audio_handle;
            ESP_LOGI(TAG, "✅ Audio Process Task created (PSRAM stack, priority=%d)",
                     AUDIO_PROCESS_TASK_PRIORITY);
        }
        else
        {
            ESP_LOGW(TAG,
                     "Failed to create audio process task (static) - wake word & cloud disabled");
            free(s_audio_process_task_stack);
            s_audio_process_task_stack   = NULL;
            s_audio_process_task_handle  = NULL;
            s_audio_process_task_running = false;
        }
    }
    else
    {
        ESP_LOGW(TAG, "Failed to allocate Audio Process Task stack - wake word & cloud disabled");
        s_audio_process_task_handle  = NULL;
        s_audio_process_task_running = false;
    }

    // Cloud Send Task will be created on-demand when cloud session is activated
    // This saves ~4KB memory during startup for AFE/I2S/WakeNet
    ESP_LOGI(TAG, "Cloud Send Task: deferred creation (will create on cloud activate)");

    ESP_LOGI(TAG, "Smart Voice Control Module started");
    return ESP_OK;
}

/**
 * @brief Stop the Smart Voice Control Module
 */
esp_err_t smart_voice_control_module_stop(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Smart Voice Control Module...");

    // Signal both audio tasks to exit
    s_i2s_reader_running         = false;
    s_audio_process_task_running = false;
    s_cloud_send_task_running    = false;

    // Reset wake state machine
    set_wake_state(WAKE_STATE_IDLE);

    // Wait for I2S reader task to finish
    if (s_i2s_reader_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_i2s_reader_task_handle = NULL;
        ESP_LOGI(TAG, "I2S Reader Task stopped");
    }

    // Wait for audio process task to finish
    if (s_audio_process_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_audio_process_task_handle = NULL;
        ESP_LOGI(TAG, "Audio Process Task stopped");

        // 方案Q: Free PSRAM stack for Audio Process Task
        if (s_audio_process_task_stack != NULL)
        {
            free(s_audio_process_task_stack);
            s_audio_process_task_stack = NULL;
            ESP_LOGI(TAG, "[PSRAM] Audio Process Task stack freed");
        }
    }

    // Wait for cloud send task to finish
    if (s_cloud_send_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_cloud_send_task_handle = NULL;
        ESP_LOGI(TAG, "Cloud Send Task stopped");

        // 方案Q: Free PSRAM stack for Cloud Send Task
        if (s_cloud_send_task_stack != NULL)
        {
            free(s_cloud_send_task_stack);
            s_cloud_send_task_stack = NULL;
            ESP_LOGI(TAG, "[PSRAM] Cloud Send Task stack freed");
        }
    }

    // Destroy PCM queue
    if (s_pcm_queue != NULL)
    {
        vQueueDelete(s_pcm_queue);
        s_pcm_queue = NULL;
        ESP_LOGI(TAG, "PCM queue destroyed");
    }

    // Destroy cloud send queue
    if (s_cloud_send_queue != NULL)
    {
        vQueueDelete(s_cloud_send_queue);
        s_cloud_send_queue = NULL;
        ESP_LOGI(TAG, "Cloud send queue destroyed");
    }

    // Stop and fully deinitialize cloud integration (must destroy chat instance for re-init)
    ESP_LOGI(TAG, "Deinitializing cloud integration...");
    cloud_integration_deinit();
    ESP_LOGI(TAG, "Cloud integration deinitialized");

    // Destroy audio processor (WakeNet/AFE)
    if (s_audio_processor != NULL)
    {
        ESP_LOGI(TAG, "Destroying audio processor...");
        audio_processor_destroy(s_audio_processor);
        s_audio_processor = NULL;
        s_feed_chunksize  = 0;
        ESP_LOGI(TAG, "Audio processor destroyed");
    }

    // Deinitialize audio driver and I2S
    ESP_LOGI(TAG, "Deinitializing audio driver...");
    audio_driver_deinit();

    ESP_LOGI(TAG, "Stopping I2S...");
    audio_i2s_stop();
    ESP_LOGI(TAG, "I2S stopped");

    // Signal main task to exit
    s_initialized = false;

    // Wait for main task to finish
    if (s_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Smart Voice Control Module stopped");
    return ESP_OK;
}

/**
 * @brief Deinitialize the Smart Voice Control Module
 */
esp_err_t smart_voice_control_module_deinit(void)
{
    // Stop module first
    smart_voice_control_module_stop();

    // Cleanup resources
    if (s_event_queue != NULL)
    {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }

    cloud_integration_deinit();
    network_monitor_deinit();
    voice_command_parser_deinit();

#if (SMART_VOICE_ENABLE_MULTINET == 1)
    multinet_deinit();
#endif

    if (s_state_mutex != NULL)
    {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }

    s_user_callback = NULL;
    s_user_data     = NULL;
    s_initialized   = false;

    ESP_LOGI(TAG, "Smart Voice Control Module deinitialized");
    return ESP_OK;
}

/**
 * @brief Push an event into the processing queue
 */
esp_err_t smart_voice_push_event(const smart_voice_event_t* event)
{
    if (event == NULL || s_event_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_event_queue, event, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Event queue full, dropping event");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * @brief Register user callback for voice events
 */
esp_err_t smart_voice_register_callback(smart_voice_event_callback_t callback, void* user_data)
{
    s_user_callback = callback;
    s_user_data     = user_data;
    return ESP_OK;
}

/**
 * @brief Get current operating mode
 */
smart_voice_mode_t smart_voice_get_mode(void)
{
    return s_current_mode;
}

/**
 * @brief Get current state
 */
smart_voice_state_t smart_voice_control_module_get_state(void)
{
    if (!s_initialized)
    {
        return SMART_VOICE_STATE_UNINIT;
    }
    if (s_task_handle == NULL)
    {
        return SMART_VOICE_STATE_INIT;
    }
    return SMART_VOICE_STATE_IDLE;
}

/**
 * @brief Get current operation mode (full API)
 */
smart_voice_mode_t smart_voice_control_module_get_mode(void)
{
    return s_current_mode;
}

/**
 * @brief Set operation mode (full API)
 */
esp_err_t smart_voice_control_module_set_mode(smart_voice_mode_t mode)
{
    if (mode >= SMART_VOICE_MODE_UNKNOWN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_current_mode = mode;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "Mode set to: %d", mode);
    return ESP_OK;
}

/**
 * @brief Get current network status
 */
smart_voice_network_status_t smart_voice_control_module_get_network_status(void)
{
    if (!s_initialized)
    {
        return SMART_VOICE_NETWORK_UNKNOWN;
    }
    if (network_monitor_is_connected())
    {
        return SMART_VOICE_NETWORK_CONNECTED;
    }
    return SMART_VOICE_NETWORK_DISCONNECTED;
}

/**
 * @brief Check if wake word is detected
 */
bool smart_voice_control_module_is_wake_word_detected(void)
{
    if (!s_initialized)
    {
        return false;
    }
    // This would be set by audio_processor callback in real implementation
    return false;
}

/**
 * @brief Get loaded command count
 */
int smart_voice_control_module_get_command_count(void)
{
    if (!s_initialized)
    {
        return 0;
    }
    return voice_command_get_count();
}

/**
 * @brief Get command by index
 */
const smart_voice_command_t* smart_voice_control_module_get_command(int index)
{
    if (!s_initialized || index < 0)
    {
        return NULL;
    }

    extern const smart_voice_command_t* voice_command_get_by_index(int idx);
    int                                 count = voice_command_get_count();
    if (index >= count)
    {
        return NULL;
    }
    return voice_command_get_by_index(index);
}

/**
 * @brief Find command by keyword
 */
const smart_voice_command_t* smart_voice_control_module_find_command(const char* keyword)
{
    if (!s_initialized || keyword == NULL)
    {
        return NULL;
    }
    return voice_command_find_by_keyword(keyword);
}

/**
 * @brief Reload configuration from file
 */
esp_err_t smart_voice_control_module_reload_config(void)
{
#if (SMART_VOICE_ENABLE_HOT_RELOAD == 1)
    return voice_command_reload_config();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Force set operating mode (for testing/debugging)
 */
esp_err_t smart_voice_set_mode(smart_voice_mode_t mode)
{
    if (mode >= SMART_VOICE_MODE_UNKNOWN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_current_mode = mode;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "Mode forced to: %d", mode);
    return ESP_OK;
}

/**
 * @brief Reload voice commands from config file
 */
esp_err_t smart_voice_reload_commands(void)
{
#if (SMART_VOICE_ENABLE_HOT_RELOAD == 1)
    return voice_command_reload_config();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Print module status information
 */
esp_err_t smart_voice_print_status(void)
{
    const char* mode_str;
    switch (s_current_mode)
    {
    case SMART_VOICE_MODE_LOCAL_ONLY:
        mode_str = "LOCAL_ONLY";
        break;
    case SMART_VOICE_MODE_CLOUD_ONLY:
        mode_str = "CLOUD_ONLY";
        break;
    case SMART_VOICE_MODE_HYBRID:
        mode_str = "HYBRID";
        break;
    default:
        mode_str = "UNKNOWN";
        break;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Smart Voice Control Module Status");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Initialized: %s", s_initialized ? "YES" : "NO");
    ESP_LOGI(TAG, "  Mode: %s", mode_str);

    if (s_initialized)
    {
        ESP_LOGI(TAG, "  Commands Loaded: %d", voice_command_get_count());
        ESP_LOGI(TAG, "  Network Status: %s",
                 network_monitor_is_connected() ? "CONNECTED" : "DISCONNECTED");
        ESP_LOGI(TAG, "  Cloud Ready: %s", cloud_integration_is_ready() ? "YES" : "NO");

        // Print sub-module statuses only when initialized
        network_monitor_print_status();
        cloud_integration_print_status();
    }
    else
    {
        ESP_LOGI(TAG, "  Commands Loaded: N/A (not initialized)");
        ESP_LOGI(TAG, "  Network Status: N/A (not initialized)");
        ESP_LOGI(TAG, "  Cloud Ready: N/A (not initialized)");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  Hint: Run 'smart_voice_init' to initialize the module");
    }

    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

/**
 * @brief Print module status (full API name)
 */
esp_err_t smart_voice_control_module_print_status(void)
{
    return smart_voice_print_status();
}

#endif // SMART_VOICE_CONTROL_MODULE_ENABLE
