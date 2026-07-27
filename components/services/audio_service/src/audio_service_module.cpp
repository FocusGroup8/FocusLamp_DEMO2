/*
 * audio_service_module.cpp - Audio service module (migrated from FocusLamp)
 *
 * Provides Opus encode/decode, audio processor (AEC/VAD/Wake Word),
 * and audio I/O task management adapted for the FocusLamp event bus architecture.
 */

#include "audio_service_module_config.h"
#include "audio_service_types.h"

#include "audio_driver.h"
#include "event_bus.h"
#include "event_def.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#if (AUDIO_SERVICE_MODULE_ENABLE == 1)
static const char* TAG = "audio_service_module";
#include "audio_processor.h"
#include "opus.h"

#define AUDIO_CONNECTED_BIT          (1 << 0)
#define AUDIO_CONNECTION_TIMEOUT_MS  (10000)

static audio_service_module_state_t     s_state     = AUDIO_SERVICE_STATE_UNINIT;
static audio_service_module_callbacks_t s_callbacks = {
    .on_send_queue_available     = NULL,
    .on_wake_word_detected       = NULL,
    .on_vad_change               = NULL,
    .on_audio_testing_queue_full = NULL,
    .on_event                    = NULL,
};

static TaskHandle_t                 s_audio_input_task_handle  = NULL;
static bool                         s_audio_input_task_running = false;
static EventGroupHandle_t           s_event_group              = NULL;

/* Opus encoder and decoder */
static OpusEncoder* s_opus_encoder = nullptr;
static OpusDecoder* s_opus_decoder = nullptr;

/* Audio processor (AEC, VAD, Wake Word) */
static audio_processor_t* s_audio_processor    = nullptr;
static bool               s_voice_detected     = false;
static bool               s_wake_word_detected = false;
static int16_t*           s_ref_buffer         = nullptr;
static int                s_ref_buffer_size    = 0;

/* Volume and gain control */
static float s_volume   = 1.0f;
static float s_gain     = 1.0f;
static bool  s_mute     = false;
static bool  s_stopping = false;

#define AUDIO_INPUT_BUFFER_SIZE       1024
#define AUDIO_INPUT_TASK_PRIORITY     5
#define AUDIO_INPUT_TASK_STACK_SIZE   4096

/* Opus parameters */
#define OPUS_SAMPLE_RATE          16000
#define OPUS_CHANNELS             1
#define OPUS_FRAME_DURATION_MS    60
#define OPUS_FRAME_SIZE           (OPUS_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000)
#define OPUS_MAX_PACKET_SIZE      4000

/* ===================== Event Bus Callbacks ===================== */

static void audio_service_event_handler(event_t *event, void *context)
{
    (void)context;
    switch (event->type) {
        case EV_AUDIO_PLAY:
            /* Forward play requests - handled externally */
            break;
        case EV_AUDIO_STOP:
            audio_service_module_stop();
            break;
        default:
            break;
    }
}

/* ===================== Audio Processor Callbacks ===================== */

static void audio_processor_output_callback(const int16_t* data, int data_size, void* user_data)
{
    (void)user_data;
    if (data == NULL || data_size <= 0 || s_opus_encoder == nullptr) {
        return;
    }

    int     samples = data_size / sizeof(int16_t);
    uint8_t opus_buffer[OPUS_MAX_PACKET_SIZE];
    int opus_bytes = opus_encode(s_opus_encoder, data, samples, opus_buffer, OPUS_MAX_PACKET_SIZE);

    if (opus_bytes > 0 && s_callbacks.on_send_queue_available) {
        /* Queue available - processed audio ready */
        s_callbacks.on_send_queue_available();
    }
}

static void audio_processor_vad_callback(audio_processor_vad_state_t vad_state, void* user_data)
{
    (void)user_data;
    bool speaking = (vad_state == AUDIO_PROCESSOR_VAD_SPEECH);
    if (speaking != s_voice_detected) {
        s_voice_detected = speaking;
        ESP_LOGI(TAG, "VAD state changed: %s", speaking ? "SPEECH" : "SILENCE");

        if (s_callbacks.on_vad_change) {
            s_callbacks.on_vad_change(speaking);
        }

        /* Publish VAD event to event bus */
        event_t ev = {
            .type = speaking ? EV_SENSOR_RADAR_DETECTED : EV_SENSOR_RADAR_CLEAR,
            .data = NULL,
            .data_size = 0,
            .timestamp = event_bus_get_timestamp(),
        };
        event_bus_publish(&ev);
    }
}

static void audio_processor_wake_word_callback(int wake_word_index, const char* wake_word_name,
                                               void* user_data)
{
    (void)user_data;
    s_wake_word_detected = true;
    ESP_LOGI(TAG, "Wake word detected: index=%d, name=%s", wake_word_index,
             wake_word_name ? wake_word_name : "unknown");

    if (s_callbacks.on_wake_word_detected) {
        s_callbacks.on_wake_word_detected(wake_word_name ? wake_word_name : "unknown");
    }
}

/* ===================== Audio Output ===================== */

static void audio_output_callback(const uint8_t* data, int len, void* ctx)
{
    (void)ctx;
    if (s_stopping) {
        return;
    }

    if (data == NULL || len <= 0) {
        return;
    }

    /* Decode Opus data to PCM */
    if (s_opus_decoder == nullptr) {
        ESP_LOGW(TAG, "Opus decoder not available");
        return;
    }

    int16_t* pcm_buffer = (int16_t*)malloc(OPUS_FRAME_SIZE * sizeof(int16_t));
    if (pcm_buffer == NULL) {
        ESP_LOGW(TAG, "Failed to allocate PCM buffer for decoding");
        return;
    }

    /* Decode Opus to PCM */
    int samples = opus_decode(s_opus_decoder, data, len, pcm_buffer, OPUS_FRAME_SIZE, 0);
    if (samples < 0) {
        ESP_LOGW(TAG, "Opus decoding failed: %s", opus_strerror(samples));
        free(pcm_buffer);
        return;
    }

    /* Convert 16-bit PCM to 32-bit I2S format for audio_driver */
    int32_t* i2s_buffer = (int32_t*)malloc(samples * sizeof(int32_t));
    if (i2s_buffer == NULL) {
        ESP_LOGW(TAG, "Failed to allocate I2S buffer");
        free(pcm_buffer);
        return;
    }

    for (int i = 0; i < samples; i++) {
        int32_t sample = (int32_t)pcm_buffer[i] << 16;

        if (s_mute) {
            i2s_buffer[i] = 0;
            continue;
        }

        int64_t processed = (int64_t)sample * s_volume * s_gain;
        if (processed > INT32_MAX) processed = INT32_MAX;
        if (processed < INT32_MIN) processed = INT32_MIN;
        i2s_buffer[i] = (int32_t)processed;
    }

    /* Write to audio driver */
    audio_driver_write((const uint8_t*)i2s_buffer, samples * sizeof(int32_t));

    free(i2s_buffer);
    free(pcm_buffer);
}

/* ===================== Audio Input Task ===================== */

static void audio_input_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Audio input task started");

    /* Allocate buffers */
    uint8_t* audio_buffer = (uint8_t*)malloc(AUDIO_INPUT_BUFFER_SIZE);
    uint8_t* opus_buffer  = (uint8_t*)malloc(OPUS_MAX_PACKET_SIZE);
    int16_t* pcm_buffer   = (int16_t*)malloc(OPUS_FRAME_SIZE * sizeof(int16_t));

    if (!audio_buffer || !opus_buffer || !pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        free(audio_buffer);
        free(opus_buffer);
        free(pcm_buffer);
        s_audio_input_task_running = false;
        s_audio_input_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    int pcm_buffer_index = 0;
    uint32_t read_count  = 0;

    while (s_audio_input_task_running) {
        /* Read audio data from driver */
        int bytes_read = audio_driver_read(audio_buffer, AUDIO_INPUT_BUFFER_SIZE);

        read_count++;

        if (bytes_read > 0) {
            /* Convert I2S data to PCM samples */
            int      samples_read = bytes_read / 4;
            int16_t* pcm_samples  = (int16_t*)audio_buffer;
            int32_t* i2s_samples  = (int32_t*)audio_buffer;

            for (int i = 0; i < samples_read && i < OPUS_FRAME_SIZE; i++) {
                int32_t sample     = i2s_samples[i] >> 16;
                int64_t processed  = (int64_t)sample * s_gain;
                if (processed > INT16_MAX) processed = INT16_MAX;
                if (processed < INT16_MIN) processed = INT16_MIN;
                pcm_samples[i] = (int16_t)processed;
            }

            /* Process through audio processor if available */
            if (s_audio_processor != nullptr && audio_processor_is_running(s_audio_processor)) {
                audio_processor_feed(s_audio_processor, pcm_samples, samples_read,
                                     s_ref_buffer, s_ref_buffer_size);
            } else {
                /* Direct encoding without audio processor */
                int samples_to_copy = samples_read;
                int src_index       = 0;

                while (samples_to_copy > 0) {
                    int space = OPUS_FRAME_SIZE - pcm_buffer_index;
                    int to_copy = (samples_to_copy < space) ? samples_to_copy : space;

                    for (int i = 0; i < to_copy; i++) {
                        pcm_buffer[pcm_buffer_index + i] = pcm_samples[src_index + i];
                    }
                    pcm_buffer_index += to_copy;
                    src_index        += to_copy;
                    samples_to_copy  -= to_copy;

                    if (pcm_buffer_index >= OPUS_FRAME_SIZE) {
                        if (s_opus_encoder != nullptr && !s_stopping) {
                            int opus_bytes = opus_encode(s_opus_encoder, pcm_buffer,
                                                         OPUS_FRAME_SIZE, opus_buffer,
                                                         OPUS_MAX_PACKET_SIZE);
                            if (opus_bytes > 0 && s_callbacks.on_send_queue_available) {
                                s_callbacks.on_send_queue_available();
                            }
                        }
                        pcm_buffer_index = 0;
                    }
                }
            }
        }
    }

    free(pcm_buffer);
    free(opus_buffer);
    free(audio_buffer);
    s_audio_input_task_handle = NULL;
    ESP_LOGI(TAG, "Audio input task stopped");
    vTaskDelete(NULL);
}

/* ===================== Public API ===================== */

extern "C" esp_err_t audio_service_module_init(const audio_service_module_config_t* config)
{
    if (s_state != AUDIO_SERVICE_STATE_UNINIT) {
        ESP_LOGW(TAG, "Audio service already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid argument: config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Audio service module initializing...");
    ESP_LOGI(TAG, "  Sample rate: %d Hz", config->sample_rate);
    ESP_LOGI(TAG, "  Channels: %d", config->channels);
    ESP_LOGI(TAG, "  Opus frame duration: %d ms", config->opus_frame_duration_ms);

    /* Create event group */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Subscribe to event bus for stop/shutdown */
    event_bus_subscribe(EV_AUDIO_STOP, audio_service_event_handler, NULL);

    /* Initialize Opus encoder */
    int opus_error;
    s_opus_encoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &opus_error);
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %s", opus_strerror(opus_error));
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ESP_FAIL;
    }

    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(5));
    ESP_LOGI(TAG, "Opus encoder created (16kHz, mono, 24kbps)");

    /* Initialize Opus decoder */
    s_opus_decoder = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &opus_error);
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(opus_error));
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = nullptr;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus decoder created (16kHz, mono)");

    /* Initialize audio processor (AEC, VAD, Wake Word) */
    if (config->enable_aec || config->enable_vad || config->enable_wake_word) {
        audio_processor_config_t processor_config = {
            .enable_aec           = config->enable_aec,
            .enable_vad           = config->enable_vad,
            .enable_wake_word     = config->enable_wake_word,
            .wake_word_model_name = "",
            .mic_channels         = 1,
            .ref_channels         = config->enable_aec ? 1 : 0,
            .sample_rate          = 16000,
        };

        s_audio_processor = audio_processor_create(&processor_config);
        if (s_audio_processor == nullptr) {
            ESP_LOGE(TAG, "Failed to create audio processor");
            opus_decoder_destroy(s_opus_decoder);
            s_opus_decoder = nullptr;
            opus_encoder_destroy(s_opus_encoder);
            s_opus_encoder = nullptr;
            vEventGroupDelete(s_event_group);
            s_event_group = NULL;
            return ESP_FAIL;
        }

        audio_processor_set_output_callback(s_audio_processor,
                                            audio_processor_output_callback, nullptr);
        audio_processor_set_vad_callback(s_audio_processor,
                                         audio_processor_vad_callback, nullptr);
        audio_processor_set_wake_word_callback(s_audio_processor,
                                               audio_processor_wake_word_callback, nullptr);

        ESP_LOGI(TAG, "Audio processor created: AEC=%s, VAD=%s, WakeWord=%s",
                 config->enable_aec ? "yes" : "no",
                 config->enable_vad ? "yes" : "no",
                 config->enable_wake_word ? "yes" : "no");

        /* Allocate reference buffer for AEC */
        if (config->enable_aec) {
            s_ref_buffer_size = audio_processor_get_feed_chunksize(s_audio_processor);
            s_ref_buffer = (int16_t*)malloc(s_ref_buffer_size * sizeof(int16_t));
            if (s_ref_buffer == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate reference buffer");
                audio_processor_destroy(s_audio_processor);
                s_audio_processor = nullptr;
                opus_decoder_destroy(s_opus_decoder);
                s_opus_decoder = nullptr;
                opus_encoder_destroy(s_opus_encoder);
                s_opus_encoder = nullptr;
                vEventGroupDelete(s_event_group);
                s_event_group = NULL;
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Reference buffer allocated: %d samples", s_ref_buffer_size);
        }
    } else {
        ESP_LOGI(TAG, "Audio processor disabled (no AEC/VAD/Wake Word)");
    }

    s_state = AUDIO_SERVICE_STATE_INIT;
    ESP_LOGI(TAG, "Audio service module initialized");

    /* Publish event */
    event_bus_publish_simple(EV_AUDIO_STATE_CHANGED);

    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_deinit(void)
{
    if (s_state == AUDIO_SERVICE_STATE_UNINIT) {
        ESP_LOGW(TAG, "Audio service not initialized");
        return ESP_OK;
    }

    /* Stop input task */
    if (s_audio_input_task_running) {
        s_audio_input_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Destroy Opus encoder and decoder */
    if (s_opus_encoder != nullptr) {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = nullptr;
    }
    if (s_opus_decoder != nullptr) {
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = nullptr;
    }

    /* Destroy audio processor */
    if (s_audio_processor != nullptr) {
        audio_processor_destroy(s_audio_processor);
        s_audio_processor = nullptr;
    }

    /* Free reference buffer */
    if (s_ref_buffer != nullptr) {
        free(s_ref_buffer);
        s_ref_buffer      = nullptr;
        s_ref_buffer_size = 0;
    }

    /* Delete event group */
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    /* Unsubscribe from event bus */
    event_bus_unsubscribe(EV_AUDIO_STOP, audio_service_event_handler);

    s_stopping = false;
    s_state    = AUDIO_SERVICE_STATE_UNINIT;
    ESP_LOGI(TAG, "Audio service module deinitialized");

    event_bus_publish_simple(EV_AUDIO_STATE_CHANGED);
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_start(void)
{
    if (s_state == AUDIO_SERVICE_STATE_UNINIT) {
        ESP_LOGE(TAG, "Audio service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == AUDIO_SERVICE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Audio service already running");
        return ESP_OK;
    }

    /* Start audio driver */
    esp_err_t ret = audio_driver_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start audio driver: %s", esp_err_to_name(ret));
    }

    /* Start audio input task */
    if (s_audio_input_task_handle == NULL && !s_audio_input_task_running) {
        s_audio_input_task_running = true;
        BaseType_t task_ret = xTaskCreate(audio_input_task, "audio_input",
                                          AUDIO_INPUT_TASK_STACK_SIZE, NULL,
                                          AUDIO_INPUT_TASK_PRIORITY,
                                          &s_audio_input_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio input task");
            s_audio_input_task_running = false;
            return ESP_FAIL;
        }
    }

    /* Start audio processor */
    if (s_audio_processor != nullptr) {
        ret = audio_processor_start(s_audio_processor);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start audio processor: %s", esp_err_to_name(ret));
        }
    }

    s_state = AUDIO_SERVICE_STATE_RUNNING;
    ESP_LOGI(TAG, "Audio service started");

    event_bus_publish_simple(EV_AUDIO_STATE_CHANGED);
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_stop(void)
{
    if (s_state != AUDIO_SERVICE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Audio service not running");
        return ESP_OK;
    }

    s_stopping = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_audio_input_task_running) {
        s_audio_input_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Stop audio processor */
    if (s_audio_processor != nullptr) {
        audio_processor_stop(s_audio_processor);
        ESP_LOGI(TAG, "Audio processor stopped");
    }

    /* Stop audio driver */
    esp_err_t ret = audio_driver_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop audio driver: %s", esp_err_to_name(ret));
    }

    s_stopping = false;
    s_state    = AUDIO_SERVICE_STATE_STOPPED;
    ESP_LOGI(TAG, "Audio service stopped");

    event_bus_publish_simple(EV_AUDIO_STATE_CHANGED);
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_push_opus_packet(const uint8_t* packet, size_t len)
{
    if (s_state != AUDIO_SERVICE_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_output_callback(packet, len, NULL);
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_pop_opus_packet(uint8_t* packet, size_t len,
                                                          size_t* actual_len)
{
    if (s_state != AUDIO_SERVICE_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)packet;
    (void)len;
    (void)actual_len;
    /* TODO: Implement Opus packet pop from queue */
    return ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t
audio_service_module_set_callbacks(const audio_service_module_callbacks_t* callbacks)
{
    if (callbacks == NULL) {
        ESP_LOGE(TAG, "Invalid argument: callbacks is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_callbacks = *callbacks;
    ESP_LOGI(TAG, "Callbacks set");
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_enable_wake_word(bool enable)
{
    (void)enable;
    ESP_LOGI(TAG, "Wake word %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_enable_voice_processing(bool enable)
{
    ESP_LOGI(TAG, "Voice processing %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

extern "C" bool audio_service_module_is_voice_detected(void)
{
    return s_voice_detected;
}

extern "C" bool audio_service_module_is_wake_word_detected(void)
{
    return s_wake_word_detected;
}

extern "C" audio_service_module_state_t audio_service_module_get_state(void)
{
    return s_state;
}

extern "C" esp_err_t audio_service_module_set_volume(float volume)
{
    if (volume < 0.0f || volume > 1.0f) {
        ESP_LOGE(TAG, "Invalid volume: %.2f", volume);
        return ESP_ERR_INVALID_ARG;
    }

    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %.2f", volume);
    return ESP_OK;
}

extern "C" esp_err_t audio_service_module_set_gain(float gain)
{
    if (gain < 0.1f || gain > 10.0f) {
        ESP_LOGE(TAG, "Invalid gain: %.2f", gain);
        return ESP_ERR_INVALID_ARG;
    }

    s_gain = gain;
    ESP_LOGI(TAG, "Gain set to %.2f", gain);
    return ESP_OK;
}

extern "C" float audio_service_module_get_volume(void)
{
    return s_volume;
}

extern "C" float audio_service_module_get_gain(void)
{
    return s_gain;
}

extern "C" esp_err_t audio_service_module_set_mute(bool mute)
{
    s_mute = mute;
    ESP_LOGI(TAG, "Mute set to: %s", mute ? "true" : "false");
    return ESP_OK;
}

extern "C" bool audio_service_module_get_mute(void)
{
    return s_mute;
}

#else /* AUDIO_SERVICE_MODULE_ENABLE */

extern "C" esp_err_t audio_service_module_init(const audio_service_module_config_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t audio_service_module_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t audio_service_module_start(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t audio_service_module_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t audio_service_module_push_opus_packet(const uint8_t* p, size_t l)
{
    (void)p; (void)l; return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t audio_service_module_pop_opus_packet(uint8_t* p, size_t l, size_t* a)
{
    (void)p; (void)l; (void)a; return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t
audio_service_module_set_callbacks(const audio_service_module_callbacks_t* c)
{
    (void)c; return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t audio_service_module_enable_wake_word(bool e) { (void)e; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t audio_service_module_enable_voice_processing(bool e) { (void)e; return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool audio_service_module_is_voice_detected(void) { return false; }
extern "C" bool audio_service_module_is_wake_word_detected(void) { return false; }
extern "C" audio_service_module_state_t audio_service_module_get_state(void)
{
    return AUDIO_SERVICE_STATE_UNINIT;
}
extern "C" esp_err_t audio_service_module_set_volume(float v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t audio_service_module_set_gain(float g) { (void)g; return ESP_ERR_NOT_SUPPORTED; }
extern "C" float audio_service_module_get_volume(void) { return 0.0f; }
extern "C" float audio_service_module_get_gain(void) { return 0.0f; }
extern "C" esp_err_t audio_service_module_set_mute(bool m) { (void)m; return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool audio_service_module_get_mute(void) { return false; }

#endif /* AUDIO_SERVICE_MODULE_ENABLE */
