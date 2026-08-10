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
static SemaphoreHandle_t s_mutex = NULL;

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
    ESP_LOGI(TAG, "AFE processing task started");

    /* Get MultiNet chunk size — detection runs when we accumulate
     * this many samples from AFE fetch output. */
    s_mn_chunk_size = s_mn_handle->get_samp_chunksize(s_mn_data);
    /* Allocate buffer with 2x capacity to safely accumulate AFE output.
     * AFE fetch chunk size may differ from MultiNet chunk size. */
    s_mn_buf_capacity = s_mn_chunk_size * 2;
    s_mn_input_buf = heap_caps_malloc(s_mn_buf_capacity * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_mn_input_buf == NULL) {
        /* Fallback to default heap (PSRAM) */
        s_mn_input_buf = heap_caps_malloc(s_mn_buf_capacity * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (s_mn_input_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MultiNet input buffer (%d samples)", s_mn_buf_capacity);
        s_afe_task_handle = NULL;
        s_afe_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    s_mn_input_samples = 0;

    ESP_LOGI(TAG, "MultiNet chunk size: %d samples, buffer capacity: %d samples",
             s_mn_chunk_size, s_mn_buf_capacity);

    while (s_afe_task_running) {
        if (!s_detecting) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Fully paused (not TTS) — no feeding, no VAD */
        if (s_paused && !s_tts_active) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Fetch processed audio from AFE (blocking with portMAX_DELAY).
         * AFE internal task processes feed data and produces output;
         * fetch_with_delay blocks until output is available, allowing
         * other tasks (including mic_task's feed) to run. */
        afe_fetch_result_t *res = s_afe_handle->fetch_with_delay(s_afe_data, portMAX_DELAY);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            if (res != NULL) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", res->ret_value);
            }
            continue;
        }

        /* Barge-in VAD: while TTS plays, run a peak VAD on the AEC-cancelled
         * output (MultiNet stays disabled to avoid the echo crash risk). */
        if (s_tts_active) {
            int n = res->data_size / sizeof(int16_t);
            int32_t peak = 0;
            for (int i = 0; i < n; i++) {
                int32_t a = res->data[i] < 0 ? -res->data[i] : res->data[i];
                if (a > peak) peak = a;
            }
            if (peak > BARGE_IN_PEAK_THRESHOLD) {
                s_barge_in_frames++;
            } else {
                s_barge_in_frames = 0;
            }
            if (s_barge_in_frames >= BARGE_IN_FRAME_COUNT && !s_barge_in_triggered) {
                s_barge_in_triggered = true;
                ESP_LOGI(TAG, "Barge-in: speech detected during TTS (peak=%ld)",
                         (long)peak);
                if (s_barge_in_cb) {
                    s_barge_in_cb(s_barge_in_cb_ctx);
                }
            }
            continue;
        }

        s_diag_fetch_count++;
        /* 临时调试日志已注释：[DIAG] fetch 统计，减少 UART 输出量 */
#if 0
        if ((s_diag_fetch_count % 50U) == 1U) {
            ESP_LOGI(TAG, "[DIAG] fetch#%lu: data_size=%d, mn_input=%d/%d",
                     (unsigned long)s_diag_fetch_count, (int)res->data_size,
                     s_mn_input_samples, s_mn_chunk_size);
        }
#endif

        /* Accumulate fetch output into MultiNet input buffer */
        int samples = res->data_size / sizeof(int16_t);
        for (int i = 0; i < samples; i++) {
            if (s_mn_input_samples < s_mn_buf_capacity) {
                s_mn_input_buf[s_mn_input_samples++] = res->data[i];
            } else {
                /* Buffer overflow — should not happen, but guard against it */
                ESP_LOGW(TAG, "MultiNet input buffer overflow, discarding %d samples", samples - i);
                break;
            }
        }

        /* Run MultiNet detect when we have enough samples.
         * Multiple detections may occur if AFE fetch chunk is larger
         * than MultiNet chunk (loop handles this). */
        while (s_mn_input_samples >= s_mn_chunk_size && s_afe_task_running) {
            /* 临时调试日志已注释：[DIAG] peak/mean 统计，减少 UART 输出量 */
#if 0
            /* Diagnostic: compute peak and mean of detect input buffer
             * to verify AEC output is not over-suppressed.
             * peak < 100 suggests AEC NLP too aggressive;
             * peak > 500 with no DETECTED suggests command word issue. */
            int32_t diag_peak = 0;
            int32_t diag_sum = 0;
            for (int i = 0; i < s_mn_chunk_size; i++) {
                int32_t v = s_mn_input_buf[i] < 0 ? -s_mn_input_buf[i] : s_mn_input_buf[i];
                if (v > diag_peak) diag_peak = v;
                diag_sum += s_mn_input_buf[i];
            }
            int32_t diag_mean = diag_sum / s_mn_chunk_size;
#endif

            esp_mn_state_t mn_state = s_mn_handle->detect(s_mn_data, s_mn_input_buf);

            /* 临时调试日志已注释：[DIAG] detect 统计打印，减少 UART 输出量 */
#if 0
            s_diag_detect_count++;
            if (mn_state == ESP_MN_STATE_DETECTING) {
                s_diag_detecting_count++;
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                s_diag_timeout_count++;
            }
            if ((s_diag_detect_count % 50U) == 1U) {
                ESP_LOGI(TAG, "[DIAG] detect#%lu: state=%d, peak=%d, mean=%d (detecting=%lu, timeout=%lu)",
                         (unsigned long)s_diag_detect_count, (int)mn_state,
                         (int)diag_peak, (int)diag_mean,
                         (unsigned long)s_diag_detecting_count,
                         (unsigned long)s_diag_timeout_count);
            }
#endif

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *results = s_mn_handle->get_results(s_mn_data);
                if (results != NULL && results->num > 0) {
                    ESP_LOGI(TAG, "Wake word detected!");
                    ESP_LOGI(TAG, "Command ID=%d, string='%s', prob=%.4f",
                             results->command_id[0], results->string, results->prob[0]);

                    s_state = WAKE_WORD_STATE_DETECTED;
                    if (s_config.detect_cb != NULL) {
                        s_config.detect_cb(results->command_id[0], results->string,
                                           s_current_lang, results->prob[0], s_config.detect_cb_ctx);
                    }
                    s_state = WAKE_WORD_STATE_LISTENING;
                }
                /* Clean MultiNet state after detection (aligned with official) */
                s_mn_handle->clean(s_mn_data);
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                /* Clean MultiNet state after timeout (aligned with official) */
                s_mn_handle->clean(s_mn_data);
                ESP_LOGD(TAG, "MultiNet timeout — back to listening");
            }
            /* ESP_MN_STATE_DETECTING: still listening, no action needed */

            /* Shift remaining samples to buffer start */
            int remaining = s_mn_input_samples - s_mn_chunk_size;
            if (remaining > 0) {
                memmove(s_mn_input_buf, &s_mn_input_buf[s_mn_chunk_size],
                        remaining * sizeof(int16_t));
            }
            s_mn_input_samples = remaining;
        }
    }

    /* Cleanup */
    if (s_mn_input_buf != NULL) {
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = NULL;
    }
    s_mn_input_samples = 0;
    s_mn_chunk_size = 0;
    s_mn_buf_capacity = 0;

    ESP_LOGI(TAG, "AFE processing task exiting");
    s_afe_task_handle = NULL;
    vTaskDelete(NULL);
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

    if (!s_detecting) {
        return;
    }
    /* Fully paused (not TTS) — stop feeding. During TTS (paused + tts_active)
     * we keep feeding mic+ref so the AFE can run AEC for barge-in VAD. */
    if (s_paused && !s_tts_active) {
        return;
    }

    if (s_afe_data == NULL || s_afe_handle == NULL) {
        return;
    }

    int feed_channels = s_afe_handle->get_feed_channel_num(s_afe_data);

    s_diag_feed_count++;
    /* 临时调试日志已注释：[DIAG] feed 统计，减少 UART 输出量 */
#if 0
    if ((s_diag_feed_count % 50U) == 1U) {
        ESP_LOGI(TAG, "[DIAG] feed#%lu: samples=%d, channels=%d",
                 (unsigned long)s_diag_feed_count, sample_count, feed_channels);
    }
#endif

    if (feed_channels == 2) {
        /* AEC mode: interleave mic + reference PCM data.
         * AFE expects [mic0, ref0, mic1, ref1, ...] format. */
        int16_t dual_buf[960 * 2];  /* Max 960 samples * 2 channels */
        int16_t ref_buf[960];

        /* Read reference signal (TTS playback) from audio_bridge ringbuf.
         * Non-blocking read: if no TTS is playing, ref_buf will be zero-filled. */
        audio_bridge_read_ref_pcm(ref_buf, sample_count, 0);

        for (int i = 0; i < sample_count; i++) {
            dual_buf[i * 2]     = pcm_data[i];  /* mic channel */
            dual_buf[i * 2 + 1] = ref_buf[i];   /* reference channel */
        }

        s_afe_handle->feed(s_afe_data, dual_buf);
    } else {
        /* Fallback: single channel (no AEC) */
        s_afe_handle->feed(s_afe_data, pcm_data);
    }
    /* No fetch here — afe_processing_task handles fetch + MultiNet detect */
}

/*---------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------*/
esp_err_t wake_word_engine_init(const wake_word_engine_config_t *config)
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

    /* Create mutex for thread safety */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Load SR models from flash partition */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "Failed to load SR models from 'model' partition. "
                 "Ensure model partition exists and srmodels.bin is flashed.");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
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
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
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
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    /* Allocate speech commands list */
    esp_err_t cmd_ret = esp_mn_commands_alloc(s_mn_handle, s_mn_data);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to alloc speech commands (may already exist): %s", esp_err_to_name(cmd_ret));
    }

    /* Allocate AFE processing task stack on internal RAM heap.
     * Using heap_caps_malloc instead of a static array to allow the
     * stack to be released on deinit. */
    int stack_size = WAKE_WORD_ENGINE_TASK_STACK;
    s_afe_task_stack = heap_caps_malloc(stack_size * sizeof(StackType_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_afe_task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate AFE task stack (%d bytes)",
                 (int)(stack_size * sizeof(StackType_t)));
        multinet_destroy();
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create AFE processing task */
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
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        heap_caps_free(s_afe_task_stack);
        s_afe_task_stack = NULL;
        s_afe_task_running = false;
        multinet_destroy();
        afe_destroy();
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
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

esp_err_t wake_word_engine_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* Stop detection first */
    s_detecting = false;
    s_paused = false;

    /* Unregister PCM callback (stop mic_task from calling pcm_callback) */
    audio_bridge_register_pcm_callback(NULL, NULL);

    /* Stop AFE processing task.
     * Use the same robust shutdown pattern as audio_bridge_mic_stop():
     * poll s_afe_task_handle until the task sets it to NULL on exit,
     * then force-delete as a last resort. */
    s_afe_task_running = false;
    int wait_ms = 0;
    while (s_afe_task_handle != NULL && wait_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }
    if (s_afe_task_handle != NULL) {
        ESP_LOGW(TAG, "AFE task did not exit in 2000ms, force deleting");
        TaskHandle_t handle = s_afe_task_handle;
        s_afe_task_handle = NULL;
        vTaskDelete(handle);
    }
    /* Free task stack (heap-allocated, not static array) */
    if (s_afe_task_stack != NULL) {
        heap_caps_free(s_afe_task_stack);
        s_afe_task_stack = NULL;
    }

    /* Free speech commands */
    esp_mn_commands_free();

    /* Free MultiNet input buffer (may be allocated by afe_processing_task) */
    if (s_mn_input_buf != NULL) {
        heap_caps_free(s_mn_input_buf);
        s_mn_input_buf = NULL;
    }
    s_mn_input_samples = 0;
    s_mn_chunk_size = 0;
    s_mn_buf_capacity = 0;

    /* Destroy models */
    multinet_destroy();
    afe_destroy();

    /* Unload SR models */
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_state = WAKE_WORD_STATE_IDLE;
    s_initialized = false;

    ESP_LOGI(TAG, "Wake word engine deinitialized");
    return ESP_OK;
}

esp_err_t wake_word_engine_start(void)
{
    if (!s_initialized) {
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

esp_err_t wake_word_engine_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_detecting = false;
    s_paused = false;
    s_state = WAKE_WORD_STATE_IDLE;

    ESP_LOGI(TAG, "Wake word detection stopped");
    return ESP_OK;
}

esp_err_t wake_word_engine_pause(void)
{
    if (!s_initialized || !s_detecting) {
        return ESP_ERR_INVALID_STATE;
    }

    s_paused = true;
    s_state = WAKE_WORD_STATE_PAUSED;

    ESP_LOGI(TAG, "Wake word detection paused");
    return ESP_OK;
}

esp_err_t wake_word_engine_resume(void)
{
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

esp_err_t wake_word_engine_set_tts_active(bool active)
{
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

esp_err_t wake_word_engine_set_language(wake_word_lang_t lang)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (lang == s_current_lang) {
        ESP_LOGW(TAG, "Already using %s", lang_to_string(lang));
        return ESP_OK;
    }

    /* Must stop detection before switching */
    bool was_detecting = s_detecting;
    s_detecting = false;

    ESP_LOGI(TAG, "Switching language: %s -> %s", lang_to_string(s_current_lang), lang_to_string(lang));

    /* Free current commands and destroy current MultiNet */
    esp_mn_commands_free();
    multinet_destroy();

    /* Create new MultiNet for target language */
    esp_err_t ret = multinet_create(lang);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to %s, attempting to restore %s",
                 lang_to_string(lang), lang_to_string(s_current_lang));
        /* Try to restore previous language */
        multinet_create(s_current_lang);
        if (was_detecting) {
            s_detecting = true;
        }
        return ESP_FAIL;
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

    /* Reset MultiNet input buffer and update chunk size for new model */
    s_mn_input_samples = 0;
    s_mn_chunk_size = s_mn_handle->get_samp_chunksize(s_mn_data);
    /* Note: s_mn_buf_capacity may need to grow if new model has larger chunk.
     * For simplicity, we keep the existing buffer; if it's too small,
     * afe_processing_task will log overflow warnings. In practice,
     * mn7_cn and mn7_en have the same chunk size. */

    /* Restore detection if it was active */
    if (was_detecting) {
        s_detecting = true;
        s_state = WAKE_WORD_STATE_LISTENING;
    }

    ESP_LOGI(TAG, "Language switched to %s", lang_to_string(lang));
    return ESP_OK;
}

wake_word_lang_t wake_word_engine_get_language(void)
{
    return s_current_lang;
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

esp_err_t wake_word_engine_add_command(int command_id, const char *phrase)
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

esp_err_t wake_word_engine_remove_command(const char *phrase)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (phrase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_mn_commands_remove(phrase);
}

esp_err_t wake_word_engine_clear_commands(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_mn_commands_clear();
}

void *wake_word_engine_update_commands(void)
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

wake_word_state_t wake_word_engine_get_state(void)
{
    return s_state;
}

esp_err_t wake_word_engine_set_threshold(float threshold)
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

void wake_word_engine_feed_pcm(const int16_t *pcm_data, int sample_count)
{
    /* This is the public API wrapper — the actual processing is done
     * via the internal pcm_callback registered with audio_bridge.
     * This function is provided for cases where the PCM callback
     * registration mechanism is not used (e.g., testing with direct calls). */
    pcm_callback(pcm_data, sample_count, NULL);
}

#endif /* WAKE_WORD_ENGINE_ENABLE */
