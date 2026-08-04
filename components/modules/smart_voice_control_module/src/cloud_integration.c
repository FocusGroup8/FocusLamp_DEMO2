#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "driver/i2s_std.h"
#include "opus.h"
#include "smart_voice_control_module.h"
#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "cloud_integration";

// Event group bits for connection state
#define CLOUD_CONNECTED_BIT (1 << 0)
#define CLOUD_DISCONNECTED_BIT (1 << 1)
#define CLOUD_CONNECTION_TIMEOUT_MS (15000) // 15 seconds timeout

// Cloud session state
static esp_xiaozhi_chat_handle_t          s_chat_handle = 0;
static volatile smart_voice_cloud_state_t s_cloud_state = SMART_VOICE_CLOUD_STATE_IDLE;
static SemaphoreHandle_t                  s_cloud_mutex = NULL;
static volatile bool      s_reconnect_needed = false; // Flag: auto-reconnect on disconnect
static EventGroupHandle_t s_event_group      = NULL;

// MCP engine for xiaozhi chat
static esp_mcp_t* s_mcp_engine = NULL;

// Opus encoder for audio compression (input)
static OpusEncoder* s_opus_encoder = NULL;
// Opus decoder for audio playback (output)
static OpusDecoder* s_opus_decoder = NULL;
// 方案Q: Use PSRAM for Opus buffer to save ~1KB Internal RAM
static uint8_t*     s_opus_buffer  = NULL;
#define OPUS_BUFFER_SIZE 1024

// Audio output parameters
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_CHANNELS 1
#define AUDIO_OUTPUT_FRAME_DURATION_MS 60 // 方案J: reverted from 120 to match 960 samples @ 16kHz
#define AUDIO_OUTPUT_FRAME_SIZE \
    (AUDIO_OUTPUT_SAMPLE_RATE * AUDIO_OUTPUT_FRAME_DURATION_MS / 1000) // 960 samples

// Callbacks
static smart_voice_cloud_callback_t s_cloud_callback  = NULL;
static void*                        s_cloud_user_data = NULL;

// Statistics
static int s_total_requests = 0;
static int s_success_count  = 0;
static int s_fail_count     = 0;

// Forward declarations (non-static for cross-file access)
esp_err_t                 cloud_integration_stop(void);
bool                      cloud_integration_is_ready(void);
smart_voice_cloud_state_t cloud_integration_get_state(void);
esp_err_t                 cloud_integration_send_wake_word(const char* wake_word);

/**
 * @brief Xiaozhi event handler for connection state changes
 */
static void xiaozhi_event_handler(void* event_handler_arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data)
{
    (void)event_handler_arg;
    (void)event_data;

    ESP_LOGI(TAG, "Xiaozhi event: base=%s, id=%ld", event_base ? (const char*)event_base : "NULL",
             event_id);

    if (s_event_group == NULL)
    {
        return;
    }

    switch (event_id)
    {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        xEventGroupSetBits(s_event_group, CLOUD_CONNECTED_BIT);
        // Restore running state and reopen audio channel if we were disconnected
        if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (s_cloud_state == SMART_VOICE_CLOUD_STATE_DISCONNECTED)
            {
                xSemaphoreGive(s_cloud_mutex);

                ESP_LOGI(TAG, "Reopening audio channel after reconnection...");

                // Small delay to ensure connection is stable
                vTaskDelay(pdMS_TO_TICKS(300));

                // Reopen audio channel
                esp_xiaozhi_chat_audio_t audio_params = {
                    .format         = "opus",
                    .sample_rate    = SMART_VOICE_SAMPLE_RATE,
                    .channels       = 1,
                    .frame_duration = SMART_VOICE_OPUS_FRAME_DURATION_MS,
                };

                esp_err_t ret =
                    esp_xiaozhi_chat_open_audio_channel(s_chat_handle, &audio_params, NULL, 0);

                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to reopen audio channel: %s", esp_err_to_name(ret));
                }
                else
                {
                    ESP_LOGI(TAG, "Audio channel reopened successfully");

                    // Restart listening mode
                    ret = esp_xiaozhi_chat_send_start_listening(
                        s_chat_handle, ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);

                    if (ret != ESP_OK)
                    {
                        ESP_LOGW(TAG, "Failed to restart listening: %s", esp_err_to_name(ret));
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Listening restarted in realtime mode");

                        if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            s_cloud_state = SMART_VOICE_CLOUD_STATE_RUNNING;
                            xSemaphoreGive(s_cloud_mutex);
                            ESP_LOGI(TAG, "Cloud state restored to RUNNING after reconnection");
                        }
                    }
                }
            }
            else
            {
                xSemaphoreGive(s_cloud_mutex);
            }
        }
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        xEventGroupSetBits(s_event_group, CLOUD_DISCONNECTED_BIT);
        // Update state to prevent audio sending attempts
        if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (s_cloud_state == SMART_VOICE_CLOUD_STATE_RUNNING)
            {
                s_cloud_state = SMART_VOICE_CLOUD_STATE_DISCONNECTED;
                ESP_LOGW(TAG, "Cloud state changed to DISCONNECTED - will auto-reconnect");
                // Set flag for auto-reconnect (handled in main task loop)
                s_reconnect_needed = true;
            }
            xSemaphoreGive(s_cloud_mutex);
        }
        break;

    default:
        break;
    }
}

/**
 * @brief Unified event callback for xiaozhi chat events
 */
static void on_xiaozhi_event(esp_xiaozhi_chat_event_t event, void* event_data, void* user_data)
{
    (void)user_data;

    if (s_cloud_callback == NULL)
    {
        return;
    }

    smart_voice_event_t voice_event;
    voice_event.timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;

    switch (event)
    {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT:
    {
        ESP_LOGI(TAG, "Cloud text response received");
        voice_event.type                 = SMART_VOICE_EVENT_CLOUD_TEXT;
        voice_event.data.cloud_text.text = "";
        voice_event.data.cloud_text.role = SMART_VOICE_TEXT_ROLE_ASSISTANT;
        s_cloud_callback(&voice_event, s_cloud_user_data);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
    {
        const char* emoji = (const char*)event_data;
        ESP_LOGI(TAG, "Cloud emoji: %s", emoji ? emoji : "(null)");
        voice_event.type                 = SMART_VOICE_EVENT_CLOUD_EMOJI;
        voice_event.data.emoji.emoji_str = emoji ? emoji : "";
        s_cloud_callback(&voice_event, s_cloud_user_data);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE:
    {
        esp_xiaozhi_chat_tts_state_t* tts_state = (esp_xiaozhi_chat_tts_state_t*)event_data;
        ESP_LOGD(TAG, "TTS state changed: %d", tts_state ? tts_state->state : -1);
        voice_event.type           = SMART_VOICE_EVENT_TTS_STATE;
        voice_event.data.tts.state = tts_state ? (int)tts_state->state : -1;
        voice_event.data.tts.text  = tts_state && tts_state->text ? tts_state->text : "";
        s_cloud_callback(&voice_event, s_cloud_user_data);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
    {
        const char* cmd = (const char*)event_data;
        ESP_LOGW(TAG, "Cloud system command: %s", cmd ? cmd : "(null)");
        voice_event.type                 = SMART_VOICE_EVENT_LOCAL_CMD;
        voice_event.data.command.keyword = cmd ? cmd : "";
        voice_event.data.command.action  = cmd ? cmd : "";
        s_cloud_callback(&voice_event, s_cloud_user_data);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR:
    {
        esp_xiaozhi_chat_error_info_t* error_info = (esp_xiaozhi_chat_error_info_t*)event_data;
        ESP_LOGE(TAG, "Chat error: code=%d, source=%s", error_info ? error_info->code : -1,
                 error_info && error_info->source ? error_info->source : "unknown");
        voice_event.type            = SMART_VOICE_EVENT_ERROR;
        voice_event.data.error.code = error_info ? error_info->code : -1;
        voice_event.data.error.message =
            error_info && error_info->source ? error_info->source : "Unknown error";
        s_cloud_callback(&voice_event, s_cloud_user_data);
        s_fail_count++;
        break;
    }

    default:
        ESP_LOGD(TAG, "Unhandled xiaozhi event type: %d", event);
        break;
    }
}

/**
 * @brief Audio callback for receiving TTS audio data from xiaozhi
 * Decodes Opus audio and outputs to I2S speaker
 */
static void on_audio_data(const uint8_t* data, int len, void* ctx)
{
    (void)ctx;

    if (data == NULL || len <= 0)
    {
        return;
    }

    static int audio_debug_count = 0;
    if (audio_debug_count < 3)
    { // Log first 3 times
        ESP_LOGI(TAG, "[TTS DEBUG] Received TTS audio: %d bytes (%d/%d)", len,
                 audio_debug_count + 1, 3);
        audio_debug_count++;
    }

    // Notify application about audio data
    if (s_cloud_callback != NULL)
    {
        smart_voice_event_t event;
        event.type              = SMART_VOICE_EVENT_AUDIO_DATA;
        event.data.audio.data   = (uint8_t*)data;
        event.data.audio.length = (size_t)len;
        event.timestamp         = xTaskGetTickCount() * portTICK_PERIOD_MS;
        s_cloud_callback(&event, s_cloud_user_data);
    }

    // Decode and play audio through I2S speaker
    if (s_opus_decoder == NULL)
    {
        ESP_LOGW(TAG, "[TTS] Opus decoder not available, skipping audio output");
        return;
    }

    // Allocate PCM buffer for decoded audio
    int16_t* pcm_buffer = (int16_t*)malloc(AUDIO_OUTPUT_FRAME_SIZE * sizeof(int16_t));
    if (pcm_buffer == NULL)
    {
        ESP_LOGW(TAG, "[TTS] Failed to allocate PCM buffer for decoding (need %zu bytes)",
                 AUDIO_OUTPUT_FRAME_SIZE * sizeof(int16_t));
        return;
    }

    // Decode Opus to PCM
    int samples = opus_decode(s_opus_decoder, data, len, pcm_buffer, AUDIO_OUTPUT_FRAME_SIZE, 0);
    if (samples < 0)
    {
        ESP_LOGW(TAG, "[TTS] Opus decoding failed: %s", opus_strerror(samples));
        free(pcm_buffer);
        return;
    }

    // Try to get I2S TX handle from audio_i2s component
    extern i2s_chan_handle_t audio_i2s_get_tx_handle(void);
    i2s_chan_handle_t        tx_handle = audio_i2s_get_tx_handle();
    if (tx_handle == NULL)
    {
        ESP_LOGW(TAG, "[TTS] I2S TX handle not available - cannot play audio!");
        free(pcm_buffer);
        return;
    }

    // Convert 16-bit PCM to 32-bit I2S format and write to I2S
    int32_t* i2s_buffer = (int32_t*)malloc(samples * sizeof(int32_t));
    if (i2s_buffer == NULL)
    {
        ESP_LOGW(TAG, "[TTS] Failed to allocate I2S buffer (need %zu bytes)",
                 samples * sizeof(int32_t));
        free(pcm_buffer);
        return;
    }

    for (int i = 0; i < samples; i++)
    {
        i2s_buffer[i] = (int32_t)pcm_buffer[i] << 16; // Expand 16-bit to 32-bit
    }

    // Write decoded audio to I2S
    size_t    bytes_written = 0;
    esp_err_t ret           = i2s_channel_write(tx_handle, i2s_buffer, samples * sizeof(int32_t),
                                                &bytes_written, pdMS_TO_TICKS(100));
    if (ret != ESP_OK)
    {
        static int write_fail_count = 0;
        if (write_fail_count < 5)
        { // Only log first 5 failures
            ESP_LOGW(TAG, "[TTS] Failed to write audio to I2S: %s (wrote %zu/%zu bytes)",
                     esp_err_to_name(ret), bytes_written, samples * sizeof(int32_t));
            write_fail_count++;
        }
    }

    free(i2s_buffer);
    free(pcm_buffer);
}

/**
 * @brief Initialize cloud integration with xiaozhi
 */
esp_err_t cloud_integration_init(void)
{
    esp_err_t ret = ESP_OK;

    // Create mutex
    if (s_cloud_mutex == NULL)
    {
        s_cloud_mutex = xSemaphoreCreateMutex();
        if (s_cloud_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create event group for connection waiting
    if (s_event_group == NULL)
    {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create event group");
            vSemaphoreDelete(s_cloud_mutex);
            s_cloud_mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    // Clear any existing bits
    xEventGroupClearBits(s_event_group, CLOUD_CONNECTED_BIT | CLOUD_DISCONNECTED_BIT);

    // Register xiaozhi event handler for connection state
    ret = esp_event_handler_instance_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                              &xiaozhi_event_handler, NULL, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register xiaozhi event handler: %s", esp_err_to_name(ret));
        // Continue anyway - we can still work without connection events
    }
    else
    {
        ESP_LOGI(TAG, "Xiaozhi event handler registered");
    }

    // Initialize Opus encoder
    int opus_error;
    s_opus_encoder =
        opus_encoder_create(SMART_VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);

    if (s_opus_encoder == NULL || opus_error != OPUS_OK)
    {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", opus_error);
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Set Opus encoding parameters
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(SMART_VOICE_OPUS_BITRATE));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    ESP_LOGI(TAG, "Opus encoder initialized (rate=%dHz, bitrate=%dbps)", SMART_VOICE_SAMPLE_RATE,
             SMART_VOICE_OPUS_BITRATE);

    // Initialize Opus decoder for audio playback
    s_opus_decoder =
        opus_decoder_create(AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_OUTPUT_CHANNELS, &opus_error);
    if (s_opus_decoder == NULL || opus_error != OPUS_OK)
    {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %d", opus_error);
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Opus decoder initialized (rate=%dHz)", AUDIO_OUTPUT_SAMPLE_RATE);

    // 方案Q: Allocate Opus buffer in PSRAM to save Internal RAM (~1KB)
    if (s_opus_buffer == NULL)
    {
        s_opus_buffer = (uint8_t*)heap_caps_malloc(OPUS_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (s_opus_buffer == NULL)
        {
            ESP_LOGW(TAG,
                     "[PSRAM] Failed to allocate Opus buffer in PSRAM, fallback to Internal RAM");
            s_opus_buffer = (uint8_t*)malloc(OPUS_BUFFER_SIZE);
        }
        else
        {
            ESP_LOGI(TAG, "[PSRAM] ✅ Opus buffer allocated in PSRAM (%d bytes)", OPUS_BUFFER_SIZE);
        }
    }

    if (s_opus_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate Opus buffer");
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create MCP engine for xiaozhi chat (required)
    ret = esp_mcp_create(&s_mcp_engine);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create MCP engine: %s", esp_err_to_name(ret));
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "MCP engine created successfully");

    // Initialize xiaozhi chat session with correct config structure
    esp_xiaozhi_chat_config_t chat_config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    chat_config.audio_type                = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    chat_config.audio_callback            = on_audio_data;
    chat_config.event_callback            = on_xiaozhi_event;
    chat_config.audio_callback_ctx        = NULL;
    chat_config.event_callback_ctx        = NULL;
    chat_config.mcp_engine                = s_mcp_engine;
    chat_config.owns_mcp_engine           = false; // We own it, we'll destroy it in deinit

    // Force use WebSocket like audio_service_module does
    chat_config.has_websocket_config = true;
    chat_config.has_mqtt_config      = false;

    ret = esp_xiaozhi_chat_init(&chat_config, &s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize xiaozhi chat: %s", esp_err_to_name(ret));
        esp_mcp_destroy(s_mcp_engine);
        s_mcp_engine = NULL;
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
        return ret;
    }

    s_cloud_state = SMART_VOICE_CLOUD_STATE_INITIALIZED;
    ESP_LOGI(TAG, "Cloud integration initialized successfully");
    return ESP_OK;
}

/**
 * @brief Deinitialize cloud integration
 */
void cloud_integration_deinit(void)
{
    // Stop session if running
    if (s_cloud_state == SMART_VOICE_CLOUD_STATE_RUNNING)
    {
        cloud_integration_stop();
    }

    // Close and destroy xiaozhi session
    if (s_chat_handle != 0)
    {
        esp_xiaozhi_chat_close_audio_channel(s_chat_handle);
        esp_xiaozhi_chat_deinit(s_chat_handle);
        s_chat_handle = 0;
    }

    // Unregister event handler
    (void)esp_event_handler_instance_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, NULL);

    // Destroy MCP engine
    if (s_mcp_engine != NULL)
    {
        esp_mcp_destroy(s_mcp_engine);
        s_mcp_engine = NULL;
    }

    // Destroy Opus encoder
    if (s_opus_encoder != NULL)
    {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
    }

    // Destroy Opus decoder
    if (s_opus_decoder != NULL)
    {
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
    }

    // 方案Q: Free Opus buffer (PSRAM or Internal RAM)
    if (s_opus_buffer != NULL)
    {
        free(s_opus_buffer);
        s_opus_buffer = NULL;
    }

    // Delete event group
    if (s_event_group != NULL)
    {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    // Delete mutex
    if (s_cloud_mutex != NULL)
    {
        vSemaphoreDelete(s_cloud_mutex);
        s_cloud_mutex = NULL;
    }

    s_cloud_callback  = NULL;
    s_cloud_user_data = NULL;
    s_cloud_state     = SMART_VOICE_CLOUD_STATE_IDLE;

    ESP_LOGI(TAG, "Cloud deinitialized. Stats: total=%d, success=%d, fail=%d", s_total_requests,
             s_success_count, s_fail_count);
}

/**
 * @brief Start cloud session (open audio channel)
 */
esp_err_t cloud_integration_start(void)
{
    if (s_chat_handle == 0)
    {
        ESP_LOGE(TAG, "Chat handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Step 0: Stop any existing session before starting new one
    // This prevents handle conflicts when reconnecting after disconnect
    EventBits_t current_bits = xEventGroupGetBits(s_event_group);
    if (current_bits & CLOUD_CONNECTED_BIT)
    {
        ESP_LOGW(TAG, "Existing connection detected, stopping before restart...");
        esp_xiaozhi_chat_stop(s_chat_handle);
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for cleanup
        // Clear the connected bit since we're intentionally disconnecting
        xEventGroupClearBits(s_event_group, CLOUD_CONNECTED_BIT);
    }

    // Step 1: Start xiaozhi chat (establishes WebSocket connection)
    ESP_LOGI(TAG, "Starting xiaozhi chat session...");
    esp_err_t ret = esp_xiaozhi_chat_start(s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start xiaozhi chat: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Xiaozhi chat started, waiting for connection...");

    // Step 2: Wait for WebSocket connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_event_group, CLOUD_CONNECTED_BIT,
                                           pdFALSE, // Don't clear bits
                                           pdFALSE, // OR wait (any bit)
                                           pdMS_TO_TICKS(CLOUD_CONNECTION_TIMEOUT_MS));

    if (!(bits & CLOUD_CONNECTED_BIT))
    {
        ESP_LOGE(TAG, "WebSocket connection timeout (%d ms)", CLOUD_CONNECTION_TIMEOUT_MS);
        // Stop the started session before returning error
        esp_xiaozhi_chat_stop(s_chat_handle);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "WebSocket connected successfully");

    // Step 3: Small delay to ensure connection is stable before opening channel
    vTaskDelay(pdMS_TO_TICKS(500));

    // Step 4: Open audio channel after connection is established (with retry)
    // Use same parameters as audio_service_module (NULL for hello msg)
    esp_xiaozhi_chat_audio_t audio_params = {
        .format         = "opus",
        .sample_rate    = SMART_VOICE_SAMPLE_RATE,
        .channels       = 1,
        .frame_duration = SMART_VOICE_OPUS_FRAME_DURATION_MS,
    };

#define MAX_AUDIO_CHANNEL_RETRIES 3
    int retry_count = 0;
    for (retry_count = 0; retry_count < MAX_AUDIO_CHANNEL_RETRIES; retry_count++)
    {
        // Check if still connected before attempting to open channel
        bits = xEventGroupGetBits(s_event_group);
        if (!(bits & CLOUD_CONNECTED_BIT))
        {
            ESP_LOGW(TAG, "WebSocket disconnected before opening audio channel, retry %d/%d...",
                     retry_count + 1, MAX_AUDIO_CHANNEL_RETRIES);

            // Try to reconnect
            esp_xiaozhi_chat_stop(s_chat_handle);
            vTaskDelay(pdMS_TO_TICKS(1000));

            ret = esp_xiaozhi_chat_start(s_chat_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Reconnection attempt %d failed: %s", retry_count + 1,
                         esp_err_to_name(ret));
                continue;
            }

            // Wait for reconnection
            bits = xEventGroupWaitBits(s_event_group, CLOUD_CONNECTED_BIT, pdFALSE, pdFALSE,
                                       pdMS_TO_TICKS(CLOUD_CONNECTION_TIMEOUT_MS));

            if (!(bits & CLOUD_CONNECTED_BIT))
            {
                ESP_LOGW(TAG, "Reconnection timeout on attempt %d", retry_count + 1);
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(500)); // Stabilize delay
        }

        ret = esp_xiaozhi_chat_open_audio_channel(
            s_chat_handle, &audio_params,
            NULL, // No hello message buffer (same as audio_service_module)
            0     // Zero size
        );

        if (ret == ESP_OK)
        {
            break; // Success!
        }

        ESP_LOGW(TAG, "Audio channel open failed (attempt %d/%d): %s", retry_count + 1,
                 MAX_AUDIO_CHANNEL_RETRIES, esp_err_to_name(ret));

        if (retry_count < MAX_AUDIO_CHANNEL_RETRIES - 1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
        }
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open audio channel after %d attempts: %s",
                 MAX_AUDIO_CHANNEL_RETRIES, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Audio channel opened successfully (attempt %d)", retry_count + 1);

    // Step 5: Start listening in realtime mode (like audio_service_module does)
    ret = esp_xiaozhi_chat_send_start_listening(s_chat_handle,
                                                ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to start listening mode: %s", esp_err_to_name(ret));
        // Not fatal - continue anyway
    }
    else
    {
        ESP_LOGI(TAG, "Listening started in realtime mode");
    }

    if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_cloud_state = SMART_VOICE_CLOUD_STATE_RUNNING;
        xSemaphoreGive(s_cloud_mutex);
    }

    ESP_LOGI(TAG, "Cloud session started (audio channel opened)");
    return ESP_OK;
}

/**
 * @brief Pre-connect to cloud (establish WebSocket/TLS connection only)
 *
 * This function should be called BEFORE starting audio processing (I2S/AFE/WakeNet)
 * to take advantage of available memory for TLS handshake.
 *
 * After pre-connect:
 * - WebSocket is connected (TLS handshake complete)
 * - Audio channel is NOT open yet
 * - Listening mode is NOT active
 * - Cloud AI is NOT processing anything
 *
 * Call cloud_integration_activate() to start the actual conversation.
 */
esp_err_t cloud_integration_preconnect(void)
{
    if (s_chat_handle == 0)
    {
        ESP_LOGE(TAG, "Chat handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_event_group == NULL)
    {
        ESP_LOGE(TAG, "Event group not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t current_bits = xEventGroupGetBits(s_event_group);
    if (current_bits & CLOUD_CONNECTED_BIT)
    {
        ESP_LOGW(TAG, "Already connected, skipping preconnect");
        if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            s_cloud_state = SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE;
            xSemaphoreGive(s_cloud_mutex);
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== Pre-connecting to cloud (TLS handshake) ===");

    esp_err_t ret = esp_xiaozhi_chat_start(s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start xiaozhi chat during preconnect: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Xiaozhi chat started, waiting for WebSocket connection...");

    EventBits_t bits = xEventGroupWaitBits(s_event_group, CLOUD_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CLOUD_CONNECTION_TIMEOUT_MS));

    if (!(bits & CLOUD_CONNECTED_BIT))
    {
        ESP_LOGE(TAG, "WebSocket connection timeout during preconnect (%d ms)",
                 CLOUD_CONNECTION_TIMEOUT_MS);
        esp_xiaozhi_chat_stop(s_chat_handle);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "WebSocket connected successfully (preconnect complete)");

    vTaskDelay(pdMS_TO_TICKS(300));

    if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_cloud_state = SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE;
        xSemaphoreGive(s_cloud_mutex);
    }

    ESP_LOGI(TAG, "Cloud preconnected: state=CONNECTED_IDLE (not listening yet)");
    return ESP_OK;
}

/**
 * @brief Activate cloud session (lightweight - assumes already pre-connected)
 *
 * This function should be called when user says "开启聊天".
 * It only does lightweight operations since TLS handshake was done during preconnect:
 * 1. Open audio channel (if not already open)
 * 2. Send wake word
 * 3. Start listening mode
 */
esp_err_t cloud_integration_activate(void)
{
    if (s_chat_handle == 0)
    {
        ESP_LOGE(TAG, "Cannot activate: chat handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    smart_voice_cloud_state_t current_state = cloud_integration_get_state();
    ESP_LOGI(TAG, "Activating cloud session (current state: %d)", current_state);

    if (current_state == SMART_VOICE_CLOUD_STATE_RUNNING)
    {
        ESP_LOGI(TAG, "Cloud already running, sending wake word only");
        return cloud_integration_send_wake_word(SMART_VOICE_WAKE_WORD);
    }

    esp_xiaozhi_chat_audio_t audio_params = {
        .format         = "opus",
        .sample_rate    = SMART_VOICE_SAMPLE_RATE,
        .channels       = 1,
        .frame_duration = SMART_VOICE_OPUS_FRAME_DURATION_MS,
    };

    EventBits_t bits = xEventGroupGetBits(s_event_group);
    if (!(bits & CLOUD_CONNECTED_BIT))
    {
        ESP_LOGW(TAG, "Not connected, attempting full start...");
        return cloud_integration_start();
    }

    esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(s_chat_handle, &audio_params, NULL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open audio channel during activate: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Audio channel opened (activate)");

    ret = esp_xiaozhi_chat_send_wake_word(s_chat_handle, SMART_VOICE_WAKE_WORD);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send wake word during activate: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Wake word sent: '%s'", SMART_VOICE_WAKE_WORD);

    ret = esp_xiaozhi_chat_send_start_listening(s_chat_handle,
                                                ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to start listening during activate: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Listening mode started (REALTIME)");
    }

    if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_cloud_state = SMART_VOICE_CLOUD_STATE_RUNNING;
        xSemaphoreGive(s_cloud_mutex);
    }

    s_total_requests++;
    ESP_LOGI(TAG, "Cloud session activated successfully");
    return ESP_OK;
}

/**
 * @brief Stop cloud session (close audio channel)
 */
esp_err_t cloud_integration_stop(void)
{
    if (s_chat_handle == 0)
    {
        return ESP_OK;
    }

    // Close audio channel first
    esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to close audio channel: %s", esp_err_to_name(ret));
    }

    // Stop xiaozhi chat session
    ret = esp_xiaozhi_chat_stop(s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to stop xiaozhi chat: %s", esp_err_to_name(ret));
    }

    // Clear connection bits
    if (s_event_group != NULL)
    {
        xEventGroupClearBits(s_event_group, CLOUD_CONNECTED_BIT);
    }

    if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_cloud_state = SMART_VOICE_CLOUD_STATE_STOPPED;
        xSemaphoreGive(s_cloud_mutex);
    }

    ESP_LOGI(TAG, "Cloud session stopped (audio channel closed)");
    return ESP_OK;
}

/**
 * @brief Send wake word notification to cloud
 *
 * Sends wake word to xiaozhi cloud to start a new conversation session.
 */
esp_err_t cloud_integration_send_wake_word(const char* wake_word)
{
    if (s_chat_handle == 0)
    {
        ESP_LOGW(TAG, "Cannot send wake word: chat handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const char* wake_str = wake_word ? wake_word : SMART_VOICE_WAKE_WORD;
    ESP_LOGI(TAG, "🎤 Sending wake word to cloud: '%s'", wake_str);

    // Ensure cloud is connected, if not try to (re)start
    if (!cloud_integration_is_ready())
    {
        ESP_LOGI(TAG, "Cloud not ready, attempting to start session...");
        esp_err_t ret = cloud_integration_start();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start cloud for wake word: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Send wake word using esp_xiaozhi API
    esp_err_t ret = esp_xiaozhi_chat_send_wake_word(s_chat_handle, wake_str);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wake word sent successfully: '%s'", wake_str);
        s_total_requests++;

        // Start listening mode after wake word
        ret = esp_xiaozhi_chat_send_start_listening(s_chat_handle,
                                                    ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to start listening after wake word: %s", esp_err_to_name(ret));
            // Not fatal - continue anyway
        }
        else
        {
            ESP_LOGI(TAG, "Listening mode started after wake word");
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send wake word: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Send audio data to cloud (with Opus compression)
 */
esp_err_t cloud_integration_send_audio(const int16_t* pcm_data, size_t pcm_len)
{
    if (s_chat_handle == 0 || !cloud_integration_is_ready() || s_opus_encoder == NULL ||
        pcm_data == NULL || pcm_len == 0)
    {
        ESP_LOGW(TAG,
                 "Invalid parameters or session not ready (handle=%lu, ready=%d, encoder=%p, "
                 "data=%p, len=%zu)",
                 (unsigned long)s_chat_handle, cloud_integration_is_ready(), s_opus_encoder,
                 pcm_data, pcm_len);
        return ESP_ERR_INVALID_ARG;
    }

    int num_samples = (int)(pcm_len / sizeof(int16_t));

    // Debug: Log first few PCM samples to diagnose encoding issues
    static int debug_counter = 0;
    if (debug_counter < 3)
    { // Only log first 3 times to avoid spam
        ESP_LOGI(TAG, "[DEBUG] Opus encode: samples=%d, first 4 values=[%d, %d, %d, %d]",
                 num_samples, pcm_data[0], pcm_data[1], pcm_data[2], pcm_data[3]);
        debug_counter++;
    }

    // Encode PCM to Opus
    int encoded_len =
        opus_encode(s_opus_encoder, (const opus_int16*)pcm_data, num_samples, s_opus_buffer,
                    OPUS_BUFFER_SIZE // 方案Q: Use constant instead of sizeof() for pointer
        );

    if (encoded_len < 0)
    {
        ESP_LOGE(TAG, "Opus encode error: %d (samples=%d, encoder=%p)", encoded_len, num_samples,
                 (void*)s_opus_encoder);

        // Additional debug: log encoder state on error
        if (encoded_len == OPUS_BAD_ARG)
        {
            ESP_LOGE(TAG, "OPUS_BAD_ARG: Invalid argument (samples=%d, expected frame_size=960)",
                     num_samples);
        }

        s_fail_count++;
        return ESP_FAIL;
    }

    // Send compressed audio to cloud using the correct API
    esp_err_t ret = esp_xiaozhi_chat_send_audio_data(s_chat_handle, (const char*)s_opus_buffer,
                                                     (size_t)encoded_len);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send audio data: %s", esp_err_to_name(ret));
        s_fail_count++;
        return ret;
    }

    s_total_requests++;
    s_success_count++;
    ESP_LOGD(TAG, "Audio sent (%d samples -> %d bytes Opus)", num_samples, encoded_len);
    return ESP_OK;
}

/**
 * @brief Get current cloud state (lock-free for high-frequency calls)
 *
 * Note: Uses direct volatile read instead of mutex for performance.
 * This is safe because:
 * 1. Single variable read is atomic on RISC-V/ESP32
 * 2. Stale values are acceptable (worst case: one extra check)
 * 3. Called 16000 times/sec in audio loop - mutex would be too slow
 */
smart_voice_cloud_state_t cloud_integration_get_state(void)
{
    return s_cloud_state; // Direct volatile read - lock-free
}

/**
 * @brief Check if cloud is connected and ready
 */
bool cloud_integration_is_ready(void)
{
    smart_voice_cloud_state_t state = cloud_integration_get_state();
    return (state == SMART_VOICE_CLOUD_STATE_RUNNING ||
            state == SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE);
}

/**
 * @brief Check if reconnection is needed and perform auto-reconnect
 *
 * Call this from main task loop to handle WebSocket disconnections.
 * This function:
 * 1. Checks if s_reconnect_needed flag is set
 * 2. Stops current session, waits briefly
 * 3. Restarts chat session to reconnect WebSocket
 * 4. Reopens audio channel if previously running
 *
 * @return ESP_OK on success or if no reconnect needed, error code on failure
 */
esp_err_t cloud_integration_check_reconnect(void)
{
    // Quick check without mutex (volatile read)
    if (!s_reconnect_needed)
    {
        return ESP_OK; // No reconnect needed
    }

    ESP_LOGI(TAG, "=== Auto-reconnecting after disconnection ===");

    // Clear flag first (prevent repeated triggers)
    s_reconnect_needed = false;

    // Check if we have a valid chat handle
    if (s_chat_handle == 0)
    {
        ESP_LOGE(TAG, "Cannot reconnect: no chat handle");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    // Step 1: Stop current session gracefully
    ESP_LOGI(TAG, "Step 1: Stopping disconnected session...");
    esp_xiaozhi_chat_stop(s_chat_handle);
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait for cleanup

    // Step 2: Restart chat session (reconnects WebSocket)
    ESP_LOGI(TAG, "Step 2: Restarting chat session...");
    ret = esp_xiaozhi_chat_start(s_chat_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to restart chat session: %s", esp_err_to_name(ret));
        // Retry once more after longer delay
        vTaskDelay(pdMS_TO_TICKS(1000));
        ret = esp_xiaozhi_chat_start(s_chat_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Retry failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    ESP_LOGI(TAG, "Chat session restarted successfully");

    // Step 3: Wait for connection event (with timeout)
    EventBits_t bits = xEventGroupWaitBits(s_event_group, CLOUD_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CLOUD_CONNECTION_TIMEOUT_MS));

    if (!(bits & CLOUD_CONNECTED_BIT))
    {
        ESP_LOGW(TAG, "Reconnection timeout - WebSocket not connected");
        return ESP_ERR_TIMEOUT;
    }

    // Step 4: Update state back to RUNNING (re-open audio channel)
    if (xSemaphoreTake(s_cloud_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_cloud_state = SMART_VOICE_CLOUD_STATE_RUNNING;
        xSemaphoreGive(s_cloud_mutex);
    }

    ESP_LOGI(TAG, "✅ Auto-reconnect successful - cloud state restored to RUNNING");
    return ESP_OK;
}

/**
 * @brief Set cloud event callback
 */
esp_err_t cloud_integration_set_callback(smart_voice_cloud_callback_t callback, void* user_data)
{

    s_cloud_callback  = callback;
    s_cloud_user_data = user_data;

    ESP_LOGD(TAG, "Cloud callback set");
    return ESP_OK;
}

/**
 * @brief Get cloud statistics
 */
esp_err_t cloud_integration_get_stats(int* total, int* success, int* fail)
{
    if (total != NULL)
        *total = s_total_requests;
    if (success != NULL)
        *success = s_success_count;
    if (fail != NULL)
        *fail = s_fail_count;
    return ESP_OK;
}

/**
 * @brief Reset statistics
 */
void cloud_integration_reset_stats(void)
{
    s_total_requests = 0;
    s_success_count  = 0;
    s_fail_count     = 0;
    ESP_LOGI(TAG, "Statistics reset");
}

/**
 * @brief Print cloud integration status
 */
esp_err_t cloud_integration_print_status(void)
{
    smart_voice_cloud_state_t state = cloud_integration_get_state();

    const char* state_str;
    switch (state)
    {
    case SMART_VOICE_CLOUD_STATE_IDLE:
        state_str = "IDLE";
        break;
    case SMART_VOICE_CLOUD_STATE_INITIALIZED:
        state_str = "INITIALIZED";
        break;
    case SMART_VOICE_CLOUD_STATE_CONNECTED_IDLE:
        state_str = "CONNECTED_IDLE";
        break;
    case SMART_VOICE_CLOUD_STATE_RUNNING:
        state_str = "RUNNING";
        break;
    case SMART_VOICE_CLOUD_STATE_STOPPED:
        state_str = "STOPPED";
        break;
    case SMART_VOICE_CLOUD_STATE_ERROR:
        state_str = "ERROR";
        break;
    default:
        state_str = "UNKNOWN";
        break;
    }

    ESP_LOGI(TAG, "=== Cloud Integration Status ===");
    ESP_LOGI(TAG, "  State: %s", state_str);
    ESP_LOGI(TAG, "  Chat Handle: %s", s_chat_handle != 0 ? "VALID" : "NULL");
    ESP_LOGI(TAG, "  Opus Encoder: %s", s_opus_encoder ? "READY" : "NULL");
    ESP_LOGI(TAG, "  Total Requests: %d", s_total_requests);
    ESP_LOGI(TAG, "  Success Rate: %.1f%%",
             s_total_requests > 0 ? (float)s_success_count / s_total_requests * 100.0f : 0.0f);
    ESP_LOGI(TAG, "================================");

    return ESP_OK;
}

#endif // SMART_VOICE_CONTROL_MODULE_ENABLE
