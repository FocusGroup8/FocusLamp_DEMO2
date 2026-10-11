/*
 * Wake Word Engine — ESP-SR MultiNet based custom wake word detection
 *
 * Architecture (aligned with official esp_xiaozhi implementation):
 *   audio_bridge mic_task → PCM callback → AFE feed (input ringbuffer)
 *   AFE internal task → AEC processing → output ringbuffer
 *   afe_processing_task → AFE fetch → MultiNet detect → detect_cb
 *
 * Key design: feed and fetch run in SEPARATE tasks. The PCM callback
 * (called from mic_task) only feeds audio to AFE. A dedicated
 * afe_processing_task fetches processed audio and runs MultiNet
 * detection. This prevents the feed/fetch deadlock that occurred when
 * both ran synchronously in mic_task context (which caused mic_task
 * to block for ~20 seconds per frame and wake word detection to fail).
 *
 * Language switching:
 *   wake_word_engine_set_language() destroys current MultiNet model and
 *   creates one for the new language. Both mn7_cn and mn7_en models
 *   are loaded from the model partition (srmodels.bin).
 */

#include "wake_word_engine.h"
#include "wake_word_engine_config.h"

#if (WAKE_WORD_ENGINE_ENABLE == 1)

/* Compile-time guard: only one MultiNet model can be enabled at a time.
 * Enabling both mn7_cn and mn7_en causes G2P conflict in esp_mn_commands_add()
 * (flite_g2p incorrectly converts Chinese pinyin to English phonemes).
 * Select only one model in menuconfig: ESP Speech Recognition -> Select MultiNet Model. */
#if defined(CONFIG_SR_MN_CN_MULTINET7_QUANT) && defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
#error "Cannot enable both mn7_cn and mn7_en simultaneously. Please select only one in menuconfig."
#endif

#include <string.h>
#include <limits.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ESP-SR headers */
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "flite_g2p.h"  /* English G2P for mn7_en command phoneme generation */

/* Audio bridge for PCM callback registration */
#include "audio_bridge.h"

static const char *TAG = "WAKE_WORD";

/*---------------------------------------------------------------
 * State variables
 *---------------------------------------------------------------*/
static bool s_initialized = false;
static volatile bool s_detecting = false;
static volatile bool s_paused = false;
static wake_word_state_t s_state = WAKE_WORD_STATE_IDLE;
static wake_word_lang_t s_current_lang = WAKE_WORD_LANG_CN;

/* Barge-in (VAD voice interrupt) state — active during TTS playback.
 * While TTS plays, the AFE keeps running with AEC; the engine runs a
 * lightweight peak VAD on the echo-cancelled output instead of MultiNet,
 * and fires the barge-in callback when sustained speech is detected. */
static volatile bool s_tts_active = false;
static volatile int s_barge_in_frames = 0;   /* consecutive VAD hits */
static volatile bool s_barge_in_triggered = false;
static wake_word_barge_in_cb_t s_barge_in_cb = NULL;
static void *s_barge_in_cb_ctx = NULL;

/* Barge-in VAD tuning: peak threshold of AEC-cancelled chunk and the number
 * of consecutive frames required. Adjust after field measurement of the AEC
 * residual level (too low → TTS echo false-triggers; too high → misses speech). */
#define BARGE_IN_PEAK_THRESHOLD 1200
#define BARGE_IN_FRAME_COUNT 3

/* Model data */
static srmodel_list_t *s_models = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static model_iface_data_t *s_mn_data = NULL;
static const esp_mn_iface_t *s_mn_handle = NULL;

/* Configuration */
static wake_word_engine_config_t s_config;
static float s_det_threshold;

/* AFE processing task state */
static TaskHandle_t s_afe_task_handle = NULL;
static volatile bool s_afe_task_running = false;
static StaticTask_t s_afe_task_tcb;
static StackType_t *s_afe_task_stack = NULL;

/* MultiNet input buffer — accumulates AFE fetch output to chunksize */
static int16_t *s_mn_input_buf = NULL;
static int s_mn_input_samples = 0;
static int s_mn_chunk_size = 0;
static int s_mn_buf_capacity = 0;  /* in samples */

/* Mutex for thread safety (API calls from multiple tasks) */
/* Retained across deinit: API waiters must never observe a deleted mutex. */
static SemaphoreHandle_t s_mutex = NULL;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_stopping;
static bool s_deinit_busy;
static bool s_afe_task_exited;
static unsigned s_feed_inflight;
static uint32_t s_audio_epoch;

static bool ensure_mutex(void)
{
    portENTER_CRITICAL(&s_mutex_init_lock);
    bool exists = s_mutex != NULL;
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (exists) return true;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return false;
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_mutex) { s_mutex = candidate; candidate = NULL; }
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (candidate) vSemaphoreDelete(candidate);
    return true;
}

static void model_lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void model_unlock(void) { xSemaphoreGive(s_mutex); }

static esp_err_t resize_mn_input_locked(void)
{
    int chunk = s_mn_handle->get_samp_chunksize(s_mn_data);
    if (chunk <= 0 || chunk > INT_MAX / (2 * (int)sizeof(int16_t)))
        return ESP_ERR_INVALID_SIZE;
    int capacity = chunk * 2;
    if (s_mn_buf_capacity < capacity) {
        int16_t *buf = heap_caps_malloc(capacity * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) buf = heap_caps_malloc(capacity * sizeof(int16_t), MALLOC_CAP_8BIT);
        if (!buf) return ESP_ERR_NO_MEM;
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = buf;
        s_mn_buf_capacity = capacity;
    }
    s_mn_chunk_size = chunk;
    s_mn_input_samples = 0;
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Diagnostic counters (for wake word detection debugging)
 *-------------------------------------------------------------*/
static volatile uint32_t s_diag_feed_count = 0;       /* pcm_callback feed invocations */
static volatile uint32_t s_diag_fetch_count = 0;      /* afe_processing_task fetch invocations */
/* 临时调试日志已注释：detect 相关计数器不再使用，避免 unused 警告 */
#if 0
static volatile uint32_t s_diag_detect_count = 0;     /* MultiNet detect invocations */
static volatile uint32_t s_diag_detecting_count = 0;  /* ESP_MN_STATE_DETECTING count */
static volatile uint32_t s_diag_timeout_count = 0;    /* ESP_MN_STATE_TIMEOUT count */
#endif

/*---------------------------------------------------------------
 * Stored commands (for re-registration on language switch)
 *
 * When wake_word_engine_set_language() is called, all registered
 * commands are freed (because the MultiNet model is destroyed and
 * recreated). To support seamless language switching, we store
 * each command with its language tag. After switching to a new
 * language, only commands matching the new language are re-added.
 *-------------------------------------------------------------*/
#define WAKE_WORD_MAX_STORED_COMMANDS 20

typedef struct {
    int command_id;
    char phrase[64];
    wake_word_lang_t lang;
    bool active;
} stored_command_t;

static stored_command_t s_stored_commands[WAKE_WORD_MAX_STORED_COMMANDS];
static int s_stored_command_count = 0;

/* Forward declaration: used by wake_word_engine_set_language() and
 * wake_word_engine_add_command() to register commands with the correct
 * phoneme generation strategy based on language. */
static esp_err_t register_command_internal(int command_id, const char *phrase, wake_word_lang_t lang);

/*---------------------------------------------------------------
 * Internal helpers
 *---------------------------------------------------------------*/
static const char *lang_to_model_name(wake_word_lang_t lang)
{
    switch (lang) {
    case WAKE_WORD_LANG_CN: return "mn7_cn";
    case WAKE_WORD_LANG_EN: return "mn7_en";
    default: return "mn7_cn";
    }
}

static const char *lang_to_string(wake_word_lang_t lang)
{
    switch (lang) {
    case WAKE_WORD_LANG_CN: return "Chinese";
    case WAKE_WORD_LANG_EN: return "English";
    default: return "Unknown";
    }
}

/*---------------------------------------------------------------
 * MultiNet model lifecycle
 *---------------------------------------------------------------*/
static esp_err_t multinet_create(wake_word_lang_t lang)
{
    const char *model_name = lang_to_model_name(lang);

    /* Get MultiNet handle for the specified language model */
    s_mn_handle = esp_mn_handle_from_name((char *)model_name);
    if (s_mn_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get MultiNet handle for '%s'", model_name);
        return ESP_FAIL;
    }

    /* Create MultiNet model instance with timeout for command recognition */
    int timeout_ms = (s_config.det_timeout_ms > 0) ? s_config.det_timeout_ms : WAKE_WORD_ENGINE_DET_TIMEOUT;
    s_mn_data = s_mn_handle->create(model_name, timeout_ms);
    if (s_mn_data == NULL) {
        ESP_LOGE(TAG, "Failed to create MultiNet model for '%s'", model_name);
        s_mn_handle = NULL;
        return ESP_FAIL;
    }

    /* Set detection threshold */
    float threshold = (s_det_threshold > 0.0f) ? s_det_threshold :
                      ((float)WAKE_WORD_ENGINE_DET_THRESHOLD / 10000.0f);
    s_mn_handle->set_det_threshold(s_mn_data, threshold);

    s_current_lang = lang;
    ESP_LOGI(TAG, "MultiNet model created: %s, threshold=%.4f, timeout=%dms",
             model_name, threshold, timeout_ms);

    return ESP_OK;
}

static void multinet_destroy(void)
{
    if (s_mn_data != NULL && s_mn_handle != NULL) {
        s_mn_handle->destroy(s_mn_data);
        s_mn_data = NULL;
        s_mn_handle = NULL;
    }
}

/*---------------------------------------------------------------
 * AFE lifecycle
 *
 * AFE configuration is aligned with the official esp_xiaozhi
 * implementation (afe_audio_engine.cc). Key differences from the
 * previous configuration:
 *   - ns_init = false: WEBRTC NS model fails on ESP32-P4, and the
 *     official implementation also disables NS for MultiNet mode.
 *   - agc_init = false: Official also disables AGC.
 *   - vad_init = false: MultiNet handles voice activity internally;
 *     we don't need AFE's VAD to gate detection.
 *   - aec_mode = AEC_MODE_VOIP_HIGH_PERF: Official uses this mode
 *     (previously we used SR_HIGH_PERF).
 *   - aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR: Official uses very
 *     aggressive NLP for better echo suppression.
 *-------------------------------------------------------------*/
static esp_err_t afe_create(void)
{
    if (s_models == NULL) {
        ESP_LOGE(TAG, "Model list not loaded");
        return ESP_FAIL;
    }

    /* Get AFE handle based on configuration */
    afe_mode_t mode = (afe_mode_t)WAKE_WORD_ENGINE_AFE_MODE;
    afe_type_t type = (afe_type_t)WAKE_WORD_ENGINE_AFE_TYPE;

    /* Input format: single microphone only (no AEC reference channel).
     * Hardware has only one mic, and AEC is disabled, so use "M" mode.
     * Previous "MR" mode with AEC disabled caused feed_channels=2 with
     * silent reference, potentially affecting AFE processing. */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, type, mode);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "Failed to init AFE config");
        return ESP_FAIL;
    }

    /* Disable WakeNet — we use MultiNet for wake word detection */
    afe_cfg->wakenet_init = false;
    afe_cfg->wakenet_model_name = NULL;

    /* AEC: DISABLED for diagnostic purposes.
     * Previous VERYAGGR over-suppressed speech (peak 223). NORMAL NLP
     * restored peak (4796) but MultiNet still cannot detect wake words,
     * suggesting AEC may be introducing speech distortion.
     * Disabled to isolate whether AEC is the root cause.
     * NOTE: TTS playback may cause false triggers without AEC.
     * If wake words work with AEC disabled, re-enable with careful tuning. */
    afe_cfg->aec_init = false;
    afe_cfg->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_cfg->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
    afe_cfg->aec_filter_length = 4;

    /* NS: disable — WEBRTC model fails on ESP32-P4, official also disables */
    afe_cfg->ns_init = false;
    afe_cfg->ns_model_name = NULL;

    /* VAD: disable — MultiNet handles voice activity internally */
    afe_cfg->vad_init = false;

    /* AGC: disable — official also disables */
    afe_cfg->agc_init = false;

    /* Use PSRAM for AFE buffers to save internal RAM */
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /* Verify and adjust configuration */
    afe_config_check(afe_cfg);

    /* Create AFE instance */
    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (s_afe_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get AFE handle from config");
        afe_config_free(afe_cfg);
        return ESP_FAIL;
    }

    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE from config");
        afe_config_free(afe_cfg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE created: mode=%d, type=%d, feed_chunk=%d samples, rate=%dHz",
             mode, type,
             s_afe_handle->get_feed_chunksize(s_afe_data),
             s_afe_handle->get_samp_rate(s_afe_data));

    afe_config_free(afe_cfg);
    return ESP_OK;
}

static void afe_destroy(void)
{
    if (s_afe_data != NULL && s_afe_handle != NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        s_afe_handle = NULL;
    }
}

/*---------------------------------------------------------------
 * AFE processing task — fetch processed audio + MultiNet detection
 *
 * This task runs independently from mic_task to avoid feed/fetch
 * deadlock. mic_task only calls feed(); this task calls fetch()
 * and runs MultiNet detection on the enhanced audio.
 *
 * The previous implementation called feed() and fetch() synchronously
 * in pcm_callback (mic_task context). This caused mic_task to block
 * for ~20 seconds per frame because:
 *   1. feed() fills AFE input ringbuffer
 *   2. fetch() immediately tries to get output, but AFE hasn't
 *      processed the input yet
 *   3. If AFE input ringbuffer is full, feed() blocks waiting for
 *      the processing task to consume data
 *   4. mic_task is stuck, cannot call i2s_channel_read()
 *   5. I2S DMA buffers accumulate, leading to long stalls
 *
 * The fix separates feed (in mic_task) and fetch (in this task),
 * allowing AFE's internal processing task to run independently.
 *-------------------------------------------------------------*/
static void afe_processing_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AFE task started: synchronized MultiNet, fetch timeout=100ms");
    for (;;) {
        model_lock();
        bool running = s_afe_task_running;
        /* Drain already admitted feeds even during pause/stop or shutdown. */
        bool fetch = s_feed_inflight > 0 ||
                     (!s_stopping && s_detecting && (!s_paused || s_tts_active));
        uint32_t epoch = s_audio_epoch;
        const esp_afe_sr_iface_t *afe = s_afe_handle;
        esp_afe_sr_data_t *data = s_afe_data;
        model_unlock();
        if (!running) break;
        if (!fetch) { vTaskDelay(1); continue; }

        /* Never hold model mutex across blocking feed/fetch. Only deinit
         * destroys AFE, after this task is parked and feeds have drained. */
        afe_fetch_result_t *res = afe->fetch_with_delay(data, pdMS_TO_TICKS(100));
        if (!res || res->ret_value == ESP_FAIL) continue;
        model_lock();
        if (s_stopping || !s_detecting || epoch != s_audio_epoch ||
            (s_paused && !s_tts_active)) {
            model_unlock();
            continue;
        }
        if (s_tts_active) {
            int n = res->data_size / sizeof(int16_t);
            int32_t peak = 0;
            for (int i = 0; i < n; ++i) {
                int32_t a = res->data[i] < 0 ? -res->data[i] : res->data[i];
                if (a > peak) peak = a;
            }
            s_barge_in_frames = peak > BARGE_IN_PEAK_THRESHOLD ?
                                s_barge_in_frames + 1 : 0;
            wake_word_barge_in_cb_t cb = NULL;
            void *ctx = NULL;
            if (s_barge_in_frames >= BARGE_IN_FRAME_COUNT && !s_barge_in_triggered) {
                s_barge_in_triggered = true;
                cb = s_barge_in_cb;
                ctx = s_barge_in_cb_ctx;
            }
            model_unlock();
            if (cb) cb(ctx);
            continue;
        }
        ++s_diag_fetch_count;
        int samples = res->data_size / sizeof(int16_t);
        int room = s_mn_buf_capacity - s_mn_input_samples;
        int count = samples < room ? samples : room;
        if (count > 0 && s_mn_input_buf) {
            memcpy(s_mn_input_buf + s_mn_input_samples, res->data,
                   count * sizeof(int16_t));
            s_mn_input_samples += count;
        }
        model_unlock();

        for (;;) {
            /* One chunk per transaction, including buffer shift and clean.
             * Give the mutex between chunks so pause/resume can intervene. */
            model_lock();
            if (s_stopping || !s_afe_task_running || !s_detecting || s_paused ||
                s_tts_active || epoch != s_audio_epoch || !s_mn_handle ||
                !s_mn_data || !s_mn_input_buf || s_mn_chunk_size <= 0 ||
                s_mn_input_samples < s_mn_chunk_size) {
                model_unlock();
                break;
            }
            int id = 0;
            float prob = 0;
            char phrase[256] = {0};
            bool detected = false;
            wake_word_lang_t lang = s_current_lang;
            wake_word_detect_cb_t cb = NULL;
            void *ctx = NULL;
            esp_mn_state_t state = s_mn_handle->detect(s_mn_data, s_mn_input_buf);
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *result = s_mn_handle->get_results(s_mn_data);
                if (result && result->num > 0) {
                    detected = true;
                    id = result->command_id[0];
                    prob = result->prob[0];
                    memcpy(phrase, result->string, sizeof(phrase) - 1);
                    cb = s_config.detect_cb;
                    ctx = s_config.detect_cb_ctx;
                    s_state = WAKE_WORD_STATE_DETECTED;
                }
            }
            if (state == ESP_MN_STATE_DETECTED || state == ESP_MN_STATE_TIMEOUT)
                s_mn_handle->clean(s_mn_data);
            int remaining = s_mn_input_samples - s_mn_chunk_size;
            if (remaining > 0)
                memmove(s_mn_input_buf, s_mn_input_buf + s_mn_chunk_size,
                        remaining * sizeof(int16_t));
            s_mn_input_samples = remaining;
            model_unlock();

            /* Callback can reenter pause/resume/language APIs. */
            if (detected) {
                ESP_LOGI(TAG, "Wake word detected: id=%d phrase='%s' prob=%.4f",
                         id, phrase, prob);
                if (cb) cb(id, phrase, lang, prob, ctx);
                model_lock();
                if (!s_stopping && epoch == s_audio_epoch &&
                    s_state == WAKE_WORD_STATE_DETECTED)
                    s_state = WAKE_WORD_STATE_LISTENING;
                model_unlock();
            }
        }
    }
    model_lock();
    s_afe_task_exited = true;
    model_unlock();
    /* Owner observes eSuspended before deleting; never self-delete or free
     * the caller-owned static task stack from this task. */
    for (;;) vTaskSuspend(NULL);
}

/*---------------------------------------------------------------
 * PCM audio callback (called from audio_bridge mic_task)
 *
 * This callback ONLY feeds audio to AFE. The fetch and MultiNet
 * detection are handled by afe_processing_task to prevent
 * feed/fetch deadlock in mic_task context.
 *-------------------------------------------------------------*/
static void pcm_callback(const int16_t *pcm_data, int sample_count, void *ctx)
{
    (void)ctx;
    if (!pcm_data || sample_count <= 0 || sample_count > 960 || !ensure_mutex()) return;
    model_lock();
    if (!s_initialized || s_stopping || !s_detecting ||
        (s_paused && !s_tts_active) || !s_afe_handle || !s_afe_data) {
        model_unlock();
        return;
    }
    const esp_afe_sr_iface_t *afe = s_afe_handle;
    esp_afe_sr_data_t *data = s_afe_data;
    int channels = afe->get_feed_channel_num(data);
    int chunk = afe->get_feed_chunksize(data);
    if (chunk <= 0 || chunk > sample_count || (channels != 1 && channels != 2)) {
        model_unlock();
        return;
    }
    ++s_feed_inflight;
    ++s_diag_feed_count;
    model_unlock();
    if (channels == 2) {
        int16_t dual_buf[960 * 2];
        int16_t ref_buf[960];
        audio_bridge_read_ref_pcm(ref_buf, sample_count, 0);
        for (int i = 0; i < sample_count; ++i) {
            dual_buf[i * 2] = pcm_data[i];
            dual_buf[i * 2 + 1] = ref_buf[i];
        }
        afe->feed(data, dual_buf);
    } else {
        afe->feed(data, pcm_data);
    }
    model_lock();
    --s_feed_inflight;
    model_unlock();
}

/*---------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------*/
static esp_err_t wake_word_engine_init_locked(const wake_word_engine_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_det_threshold = (config->det_threshold > 0.0f) ? config->det_threshold :
                      ((float)WAKE_WORD_ENGINE_DET_THRESHOLD / 10000.0f);
    s_barge_in_cb = config->barge_in_cb;
    s_barge_in_cb_ctx = config->barge_in_cb_ctx;


    /* Load SR models from flash partition */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "Failed to load SR models from 'model' partition. "
                 "Ensure model partition exists and srmodels.bin is flashed.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SR models loaded: %d models available", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  Model[%d]: %s", i, s_models->model_name[i]);
    }

    /* Create AFE */
    esp_err_t ret = afe_create();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create AFE");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ret;
    }

    /* Create MultiNet model */
    wake_word_lang_t lang = (config->default_lang == WAKE_WORD_LANG_EN) ?
                            WAKE_WORD_LANG_EN : WAKE_WORD_LANG_CN;
    ret = multinet_create(lang);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MultiNet");
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ret;
    }

    /* Allocate speech commands list */
    esp_err_t cmd_ret = esp_mn_commands_alloc(s_mn_handle, s_mn_data);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to alloc speech commands (may already exist): %s", esp_err_to_name(cmd_ret));
    }

    ret = resize_mn_input_locked();
    if (ret != ESP_OK) {
        esp_mn_commands_free();
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = NULL;
        s_mn_buf_capacity = s_mn_chunk_size = s_mn_input_samples = 0;
        multinet_destroy();
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ret;
    }

    /* Allocate AFE processing task stack on internal RAM heap.
     * Using heap_caps_malloc instead of a static array to allow the
     * stack to be released on deinit. */
    int stack_size = WAKE_WORD_ENGINE_TASK_STACK;
    s_afe_task_stack = heap_caps_malloc(stack_size * sizeof(StackType_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_afe_task_stack == NULL) {
        esp_mn_commands_free();
        ESP_LOGE(TAG, "Failed to allocate AFE task stack (%d bytes)",
                 (int)(stack_size * sizeof(StackType_t)));
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = NULL;
        s_mn_buf_capacity = s_mn_chunk_size = s_mn_input_samples = 0;
        multinet_destroy();
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create AFE processing task */
    s_stopping = false;
    s_afe_task_exited = false;
    s_detecting = s_paused = s_tts_active = false;
    s_barge_in_frames = 0;
    s_barge_in_triggered = false;
    s_stored_command_count = 0;
    ++s_audio_epoch;
    s_afe_task_running = true;
    BaseType_t core = (WAKE_WORD_ENGINE_TASK_CORE < 0) ? tskNO_AFFINITY : WAKE_WORD_ENGINE_TASK_CORE;
    memset(&s_afe_task_tcb, 0, sizeof(s_afe_task_tcb));

    s_afe_task_handle = xTaskCreateStaticPinnedToCore(
        afe_processing_task,
        "afe_proc",
        stack_size,
        NULL,
        WAKE_WORD_ENGINE_TASK_PRIORITY,
        s_afe_task_stack,
        &s_afe_task_tcb,
        core
    );

    if (s_afe_task_handle == NULL) {
        esp_mn_commands_free();
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        heap_caps_free(s_afe_task_stack);
        s_afe_task_stack = NULL;
        s_afe_task_running = false;
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = NULL;
        s_mn_buf_capacity = s_mn_chunk_size = s_mn_input_samples = 0;
        multinet_destroy();
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    s_state = WAKE_WORD_STATE_IDLE;
    s_initialized = true;

    /* Register PCM callback with audio_bridge.
     * This must be called AFTER s_initialized = true so that pcm_callback
     * (which checks s_detecting) can safely run once mic_task starts. */
    audio_bridge_register_pcm_callback(pcm_callback, NULL);

    ESP_LOGI(TAG, "Wake word engine initialized (lang=%s)", lang_to_string(lang));
    return ESP_OK;
}

esp_err_t wake_word_engine_init(const wake_word_engine_config_t *config)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_init_locked(config);
    model_unlock();
    return result;
}

esp_err_t wake_word_engine_deinit(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    /* A stalled detect transaction must not turn shutdown into an
     * unbounded wait. No resource may be destroyed without this lock. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wake shutdown could not acquire model lock; resources retained");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_initialized) { model_unlock(); return ESP_OK; }
    if (s_deinit_busy || xTaskGetCurrentTaskHandle() == s_afe_task_handle) {
        model_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_deinit_busy = true;
    s_stopping = true;
    s_detecting = false;
    s_paused = false;
    s_tts_active = false;
    ++s_audio_epoch;
    s_state = WAKE_WORD_STATE_IDLE;
    model_unlock();

    esp_err_t ret = audio_bridge_unregister_pcm_callback_and_wait(2000);
    if (ret != ESP_OK) goto deferred;
    /* Public direct-feed callers are also counted. Keep fetch running
     * until admitted feeds finish; never wait while holding model mutex. */
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        model_lock();
        unsigned feeds = s_feed_inflight;
        model_unlock();
        if (!feeds) break;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(2000)) {
            ret = ESP_ERR_TIMEOUT;
            goto deferred;
        }
        vTaskDelay(1);
    }
    model_lock();
    s_afe_task_running = false;
    TaskHandle_t task = s_afe_task_handle;
    model_unlock();
    start = xTaskGetTickCount();
    for (;;) {
        model_lock();
        bool exited = s_afe_task_exited;
        model_unlock();
        /* Exited flag alone is insufficient: it precedes suspension. The
         * private task cannot be resumed by any public API during stopping. */
        if (!task || (exited && eTaskGetState(task) == eSuspended)) break;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(2000)) {
            ret = ESP_ERR_TIMEOUT;
            goto deferred;
        }
        vTaskDelay(1);
    }
    if (task) vTaskDelete(task); /* already parked, not running on either core */
    model_lock();
    s_afe_task_handle = NULL;
    heap_caps_free(s_afe_task_stack);
    s_afe_task_stack = NULL;
    heap_caps_free(s_mn_input_buf);
    s_mn_input_buf = NULL;
    s_mn_input_samples = s_mn_chunk_size = s_mn_buf_capacity = 0;
    esp_mn_commands_free();
    multinet_destroy();
    afe_destroy();
    if (s_models) esp_srmodel_deinit(s_models);
    s_models = NULL;
    s_stored_command_count = 0;
    memset(s_stored_commands, 0, sizeof(s_stored_commands));
    s_initialized = false;
    s_stopping = s_deinit_busy = false;
    s_state = WAKE_WORD_STATE_IDLE;
    model_unlock();
    ESP_LOGI(TAG, "Wake word engine deinitialized: feeds drained, task parked");
    return ESP_OK;

deferred:
    model_lock();
    s_deinit_busy = false;
    model_unlock();
    ESP_LOGE(TAG, "Wake shutdown incomplete (%s); resources retained, retry deinit",
             esp_err_to_name(ret));
    return ret;
}

static esp_err_t wake_word_engine_start_locked(void)
{
    if (!s_initialized || !s_mn_handle || !s_mn_data) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_detecting) {
        ESP_LOGW(TAG, "Already detecting");
        return ESP_OK;
    }

    s_detecting = true;
    s_paused = false;
    s_state = WAKE_WORD_STATE_LISTENING;

    ESP_LOGI(TAG, "Wake word detection started (lang=%s)", lang_to_string(s_current_lang));
    return ESP_OK;
}

esp_err_t wake_word_engine_start(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_start_locked();
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_stop_locked(void)
{
    ++s_audio_epoch;
    s_mn_input_samples = 0;
    if (!s_initialized) {
        return ESP_OK;
    }

    s_detecting = false;
    s_paused = false;
    s_state = WAKE_WORD_STATE_IDLE;

    ESP_LOGI(TAG, "Wake word detection stopped");
    return ESP_OK;
}

esp_err_t wake_word_engine_stop(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_stop_locked();
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_pause_locked(void)
{
    ++s_audio_epoch;
    s_mn_input_samples = 0;
    if (!s_initialized || !s_detecting) {
        return ESP_ERR_INVALID_STATE;
    }

    s_paused = true;
    s_state = WAKE_WORD_STATE_PAUSED;

    ESP_LOGI(TAG, "Wake word detection paused");
    return ESP_OK;
}

esp_err_t wake_word_engine_pause(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_pause_locked();
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_resume_locked(void)
{
    ++s_audio_epoch;
    s_mn_input_samples = 0;
    if (!s_initialized || !s_detecting) {
        return ESP_ERR_INVALID_STATE;
    }

    s_paused = false;
    s_state = WAKE_WORD_STATE_LISTENING;

    /* Clean MultiNet state to avoid stale detection after resume */
    if (s_mn_handle != NULL && s_mn_data != NULL) {
        s_mn_handle->clean(s_mn_data);
    }
    /* Reset input buffer to discard audio accumulated during pause */
    s_mn_input_samples = 0;

    ESP_LOGI(TAG, "Wake word detection resumed");
    return ESP_OK;
}

esp_err_t wake_word_engine_resume(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_resume_locked();
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_set_tts_active_locked(bool active)
{
    ++s_audio_epoch;
    s_mn_input_samples = 0;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_tts_active = active;
    s_barge_in_frames = 0;
    s_barge_in_triggered = false;

    if (active) {
        ESP_LOGI(TAG, "TTS active: barge-in VAD enabled (threshold=%d, frames=%d)",
                 BARGE_IN_PEAK_THRESHOLD, BARGE_IN_FRAME_COUNT);
    } else {
        ESP_LOGI(TAG, "TTS inactive: barge-in VAD disabled");
    }
    return ESP_OK;
}

esp_err_t wake_word_engine_set_tts_active(bool active)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_set_tts_active_locked(active);
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_set_language_locked(wake_word_lang_t lang)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (lang != WAKE_WORD_LANG_CN && lang != WAKE_WORD_LANG_EN)
        return ESP_ERR_INVALID_ARG;
    if (lang == s_current_lang) {
        ESP_LOGW(TAG, "Already using %s", lang_to_string(lang));
        return ESP_OK;
    }

    /* Must stop detection before switching */
    wake_word_lang_t old_lang = s_current_lang;
    bool was_detecting = s_detecting;
    ++s_audio_epoch;
    s_detecting = false;

    ESP_LOGI(TAG, "Switching language: %s -> %s", lang_to_string(s_current_lang), lang_to_string(lang));

    /* Free current commands and destroy current MultiNet */
    esp_mn_commands_free();
    multinet_destroy();

    /* Create new MultiNet for target language */
    esp_err_t ret = multinet_create(lang);
    if (ret == ESP_OK) ret = resize_mn_input_locked();
    bool restored = false;
    if (ret != ESP_OK) {
        multinet_destroy();
        if (multinet_create(old_lang) != ESP_OK ||
            resize_mn_input_locked() != ESP_OK) {
            s_state = WAKE_WORD_STATE_IDLE;
            return ret;
        }
        lang = old_lang;
        restored = true;
    }

    /* Allocate new commands list */
    esp_mn_commands_alloc(s_mn_handle, s_mn_data);

    /* Re-register commands matching the new language.
     * Commands are stored with their original language tag;
     * only commands matching the new language are re-added.
     * This allows users to pre-register commands for both languages
     * (e.g., CN commands "ni hao xiao zhi" + EN commands "focus")
     * and switch between them without re-adding each time. */
    int re_added = 0;
    for (int i = 0; i < s_stored_command_count; i++) {
        if (s_stored_commands[i].active && s_stored_commands[i].lang == lang) {
            esp_err_t cmd_ret = register_command_internal(
                s_stored_commands[i].command_id,
                s_stored_commands[i].phrase,
                lang);
            if (cmd_ret == ESP_OK) {
                re_added++;
            } else {
                ESP_LOGW(TAG, "Failed to re-add command '%s' after language switch",
                         s_stored_commands[i].phrase);
            }
        }
    }
    if (re_added > 0) {
        esp_mn_commands_update();
    }
    ESP_LOGI(TAG, "Re-registered %d command(s) for %s", re_added, lang_to_string(lang));

    /* Restore detection if it was active */
    if (was_detecting) {
        s_detecting = true;
        s_state = WAKE_WORD_STATE_LISTENING;
    }

    ESP_LOGI(TAG, "Language switched to %s", lang_to_string(lang));
    return restored ? ret : ESP_OK;
}

esp_err_t wake_word_engine_set_language(wake_word_lang_t lang)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_set_language_locked(lang);
    model_unlock();
    return result;
}

static wake_word_lang_t wake_word_engine_get_language_locked(void)
{
    return s_current_lang;
}

wake_word_lang_t wake_word_engine_get_language(void)
{
    if (!ensure_mutex()) return WAKE_WORD_LANG_CN;
    model_lock();
    wake_word_lang_t result = wake_word_engine_get_language_locked();
    model_unlock();
    return result;
}

/*---------------------------------------------------------------
 * Internal: register command with ESP-SR based on language
 *
 * Uses esp_mn_commands_phoneme_add() instead of esp_mn_commands_add()
 * to bypass the compile-time #ifdef in ESP-SR that forces ALL commands
 * through English flite_g2p() when CONFIG_SR_MN_EN_MULTINET7_QUANT is
 * defined. This would corrupt Chinese pinyin (e.g., "ni hao xiao zhi"
 * gets English G2P conversion, producing wrong phonemes that mn7_cn
 * rejects).
 *
 * By calling phoneme_add() directly, we control phoneme generation:
 *   - CN model: pinyin IS the phoneme unit (pass phrase as phonemes)
 *   - EN model: call flite_g2p() to convert English text to phonemes
 *-------------------------------------------------------------*/
static esp_err_t register_command_internal(int command_id, const char *phrase, wake_word_lang_t lang)
{
    esp_err_t ret;
    if (lang == WAKE_WORD_LANG_EN) {
        /* English: convert graphemes to phonemes via flite_g2p */
        char *phonemes = flite_g2p(phrase, 1);
        if (phonemes == NULL) {
            ESP_LOGE(TAG, "flite_g2p failed for '%s'", phrase);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Adding EN command: id=%d, phrase='%s', phonemes='%s'",
                 command_id, phrase, phonemes);
        ret = esp_mn_commands_phoneme_add(command_id, phrase, phonemes);
        free(phonemes);
    } else {
        /* Chinese: pinyin IS the phoneme unit for mn7_cn.
         * Pass pinyin as both string and phonemes. */
        ESP_LOGI(TAG, "Adding CN command: id=%d, phrase='%s' (pinyin as phoneme)",
                 command_id, phrase);
        ret = esp_mn_commands_phoneme_add(command_id, phrase, phrase);
    }
    return ret;
}

static esp_err_t wake_word_engine_add_command_locked(int command_id, const char *phrase)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (phrase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Store command for re-registration on language switch.
     * Each command is tagged with the current language so that
     * wake_word_engine_set_language() knows which commands to re-add. */
    if (s_stored_command_count < WAKE_WORD_MAX_STORED_COMMANDS) {
        s_stored_commands[s_stored_command_count].command_id = command_id;
        strncpy(s_stored_commands[s_stored_command_count].phrase, phrase, 63);
        s_stored_commands[s_stored_command_count].phrase[63] = '\0';
        s_stored_commands[s_stored_command_count].lang = s_current_lang;
        s_stored_commands[s_stored_command_count].active = true;
        s_stored_command_count++;
    } else {
        ESP_LOGW(TAG, "Command storage full, cannot store '%s' for re-registration", phrase);
    }

    return register_command_internal(command_id, phrase, s_current_lang);
}

esp_err_t wake_word_engine_add_command(int command_id, const char *phrase)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_add_command_locked(command_id, phrase);
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_remove_command_locked(const char *phrase)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (phrase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_mn_commands_remove(phrase);
    if (ret == ESP_OK) {
        for (int i = 0; i < s_stored_command_count; ++i)
            if (s_stored_commands[i].lang == s_current_lang &&
                strcmp(s_stored_commands[i].phrase, phrase) == 0)
                s_stored_commands[i].active = false;
    }
    return ret;
}

esp_err_t wake_word_engine_remove_command(const char *phrase)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_remove_command_locked(phrase);
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_clear_commands_locked(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_mn_commands_clear();
    if (ret == ESP_OK) {
        for (int i = 0; i < s_stored_command_count; ++i)
            if (s_stored_commands[i].lang == s_current_lang)
                s_stored_commands[i].active = false;
    }
    return ret;
}

esp_err_t wake_word_engine_clear_commands(void)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_clear_commands_locked();
    model_unlock();
    return result;
}

static void *wake_word_engine_update_commands_locked(void)
{
    if (!s_initialized) {
        return NULL;
    }

    void *err = esp_mn_commands_update();

    /* Diagnostic: print all registered speech commands to verify
     * that mn7_cn correctly parsed the pinyin strings.
     * Aligned with official esp_xiaozhi custom_wake_word.cc L129. */
    if (s_mn_handle != NULL && s_mn_data != NULL) {
        ESP_LOGI(TAG, "=== Active speech commands ===");
        s_mn_handle->print_active_speech_commands(s_mn_data);
        ESP_LOGI(TAG, "=== End of speech commands ===");
    }

    return err;
}

void * wake_word_engine_update_commands(void)
{
    if (!ensure_mutex()) return NULL;
    model_lock();
    if (s_stopping) { model_unlock(); return NULL; }
    void * result = wake_word_engine_update_commands_locked();
    model_unlock();
    return result;
}

void wake_word_engine_free_error(void *err)
{
    if (err == NULL) {
        return;
    }
    esp_mn_error_t *mn_err = (esp_mn_error_t *)err;
    ESP_LOGW(TAG, "Freeing %d unrecognized command phrase(s)", mn_err->num);
    for (int i = 0; i < mn_err->num; i++) {
        ESP_LOGW(TAG, "  Error phrase[%d]: '%s'", i, mn_err->phrases[i]->string);
    }
    free(mn_err->phrases);
    free(mn_err);
}

static wake_word_state_t wake_word_engine_get_state_locked(void)
{
    return s_state;
}

wake_word_state_t wake_word_engine_get_state(void)
{
    if (!ensure_mutex()) return WAKE_WORD_STATE_IDLE;
    model_lock();
    wake_word_state_t result = wake_word_engine_get_state_locked();
    model_unlock();
    return result;
}

static esp_err_t wake_word_engine_set_threshold_locked(float threshold)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (threshold < 0.0f || threshold > 0.9999f) {
        return ESP_ERR_INVALID_ARG;
    }

    s_det_threshold = threshold;
    if (s_mn_handle != NULL && s_mn_data != NULL) {
        s_mn_handle->set_det_threshold(s_mn_data, threshold);
    }

    ESP_LOGI(TAG, "Detection threshold set to %.4f", threshold);
    return ESP_OK;
}

esp_err_t wake_word_engine_set_threshold(float threshold)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    model_lock();
    if (s_stopping) { model_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t result = wake_word_engine_set_threshold_locked(threshold);
    model_unlock();
    return result;
}

void wake_word_engine_feed_pcm(const int16_t *pcm_data, int sample_count)
{
    /* This is the public API wrapper — the actual processing is done
     * via the internal pcm_callback registered with audio_bridge.
     * This function is provided for cases where the PCM callback
     * registration mechanism is not used (e.g., testing with direct calls). */
    pcm_callback(pcm_data, sample_count, NULL);
}

#endif /* WAKE_WORD_ENGINE_ENABLE */
