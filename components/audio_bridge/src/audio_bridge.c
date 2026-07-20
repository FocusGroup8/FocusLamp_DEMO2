/*
 * Audio Bridge Component
 *
 * Bridges esp_xiaozhi's audio_callback (OPUS-encoded TTS data) to I2S hardware playback.
 * Also provides a microphone recording interface for future voice input.
 *
 * I2S init pattern migrated from mipi_dsi project's i2s_driver.c.
 * OPUS decoding uses espressif/esp_audio_codec (esp_opus_dec API).
 *
 * Architecture: OPUS decoding runs in a dedicated audio_decode_task with static
 * allocation and internal DRAM stack. This ensures PIE coprocessor (SIMD) context
 * is properly managed by FreeRTOS lazy context switching, preventing MCAUSE 0x1f
 * exceptions that occurred when decoding ran in the WebSocket client task context.
 */

#include "audio_bridge.h"
#include "audio_bridge_config.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "riscv/rv_utils.h"
#include "soc/soc_caps.h"

#if (AUDIO_BRIDGE_ENABLE == 1)
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_audio_types.h"
#endif

static const char *TAG = "AUDIO_BRIDGE";

#if (AUDIO_BRIDGE_ENABLE == 1)

/*---------------------------------------------------------------
 * Constants and configuration
 *-------------------------------------------------------------*/
/* OPUS frame max payload: typical OPUS 16kHz mono 60ms frame is 100-400 bytes,
 * but server may concatenate or send larger chunks. Use a safe upper bound. */
#define OPUS_FRAME_MAX_SIZE     1500

/* Decode task queue depth: enough to buffer several frames while I2S drains.
 * Each queue item carries a pointer to a dynamically allocated buffer,
 * so memory usage = depth * sizeof(void*) for the queue itself +
 * depth * OPUS_FRAME_MAX_SIZE for the data buffers (allocated on demand). */
#define DECODE_QUEUE_DEPTH      8

/* Decode task configuration — must be large enough for OPUS decoder stack +
 * FPU/PIE/HWLOOP coprocessor lazy save areas (allocated from task stack bottom).
 * ESP32-P4: FPU=136B, PIE=~200B, HWLOOP=~20B. With 8KB stack the coprocessor
 * areas fit comfortably. */
#define DECODE_TASK_STACK_SIZE  CONFIG_AUDIO_BRIDGE_DECODE_TASK_STACK_SIZE
#define DECODE_TASK_PRIORITY    CONFIG_AUDIO_BRIDGE_DECODE_TASK_PRIORITY
#define DECODE_TASK_CORE        CONFIG_AUDIO_BRIDGE_DECODE_TASK_CORE

#define DECODE_TASK_NAME        "audio_dec"

/*---------------------------------------------------------------
 * I2S state
 *-------------------------------------------------------------*/
static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static bool s_initialized = false;
static int s_volume = CONFIG_AUDIO_BRIDGE_DEFAULT_VOLUME;
static audio_bridge_config_t s_config;

/*---------------------------------------------------------------
 * OPUS decoder state
 *-------------------------------------------------------------*/
static void *s_opus_decoder = NULL;

/* OPUS解码输出缓冲区：16kHz, mono, 60ms帧 = 960 samples * 2 bytes = 1920 bytes */
#define OPUS_DEC_OUT_BUF_SIZE 2048
static int16_t *s_opus_out_buf = NULL;

/* I2S 32-bit转换缓冲区：最大960 samples * 4 bytes = 3840 bytes */
#define PCM32_BUF_SIZE (960 * sizeof(int32_t))
static int32_t *s_pcm32_buf = NULL;

/*---------------------------------------------------------------
 * Decode task state
 *-------------------------------------------------------------*/
static TaskHandle_t s_decode_task_handle = NULL;
static QueueHandle_t s_decode_queue = NULL;
static volatile bool s_decode_task_running = false;

/* Static allocation for decode task (avoids heap fragmentation) */
static StaticTask_t s_decode_task_tcb;
static StackType_t s_decode_task_stack[DECODE_TASK_STACK_SIZE];

/*---------------------------------------------------------------
 * TTS Reference signal ring buffer for AEC
 *
 * When AEC is enabled, write_pcm_to_i2s() also pushes the 16-bit PCM
 * data (after volume scaling) into this ring buffer. The wake word
 * engine reads from it to feed AFE's reference channel.
 *
 * Buffer size: ~1 second of 16kHz mono PCM = 32000 bytes.
 *-------------------------------------------------------------*/
#include "freertos/ringbuf.h"
static RingbufHandle_t s_ref_ringbuf = NULL;
#define REF_RINGBUF_SIZE  (16000 * 2)  /* 1 second of 16-bit 16kHz mono */

/*---------------------------------------------------------------
 * OPUS decoder helpers
 *-------------------------------------------------------------*/
static esp_err_t opus_decoder_create(void)
{
    if (s_opus_decoder != NULL) {
        ESP_LOGW(TAG, "OPUS decoder already created");
        return ESP_OK;
    }

    /* OPUS decoder 内部使用 malloc() 分配约 30-40KB 内存。
     * 由于 CONFIG_SPIRAM_USE_MALLOC=y，默认 malloc 会将 >16KB 的分配导向 PSRAM。
     * 但 PIE SIMD 浮点运算（opus_dot16_32_opt 等）访问 PSRAM 数据可能触发
     * ESP32-P4 异步总线错误 (MCAUSE=0x1f)。
     * 解决方案：临时禁用外部内存分配（设置阈值为 -1），
     * 强制所有 malloc 分配到内部 DRAM，确保 OPUS 解码器工作数据在内部 RAM 中。
     * 创建完成后恢复原始阈值。 */
#define MALLOC_DISABLE_EXTERNAL_ALLOCS_NEG1 ((size_t)-1)
    size_t saved_limit = CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
    heap_caps_malloc_extmem_enable(MALLOC_DISABLE_EXTERNAL_ALLOCS_NEG1);

    esp_opus_dec_cfg_t cfg = {
        .sample_rate    = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel        = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };

    esp_audio_err_t ret = esp_opus_dec_open(&cfg, sizeof(cfg), &s_opus_decoder);

    /* 恢复原始 MALLOC_ALWAYSINTERNAL 阈值 */
    heap_caps_malloc_extmem_enable(saved_limit);

    if (ret != ESP_AUDIO_ERR_OK || s_opus_decoder == NULL) {
        ESP_LOGE(TAG, "Failed to open OPUS decoder: %d", ret);
        s_opus_decoder = NULL;
        return ESP_FAIL;
    }

    /* OPUS 输出缓冲区和 PCM32 转换缓冲区也强制分配在内部 DRAM */
    s_opus_out_buf = heap_caps_malloc(OPUS_DEC_OUT_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_opus_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OPUS output buffer (%d bytes) in internal RAM", OPUS_DEC_OUT_BUF_SIZE);
        esp_opus_dec_close(s_opus_decoder);
        s_opus_decoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_pcm32_buf = heap_caps_malloc(PCM32_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_pcm32_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM32 buffer (%d bytes) in internal RAM", (int)PCM32_BUF_SIZE);
        heap_caps_free(s_opus_out_buf);
        s_opus_out_buf = NULL;
        esp_opus_dec_close(s_opus_decoder);
        s_opus_decoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OPUS decoder created (16kHz, mono, 60ms frame, internal RAM)");
    return ESP_OK;
}

static void opus_decoder_destroy(void)
{
    if (s_opus_decoder != NULL) {
        esp_opus_dec_close(s_opus_decoder);
        s_opus_decoder = NULL;
    }
    if (s_opus_out_buf != NULL) {
        heap_caps_free(s_opus_out_buf);
        s_opus_out_buf = NULL;
    }
    if (s_pcm32_buf != NULL) {
        heap_caps_free(s_pcm32_buf);
        s_pcm32_buf = NULL;
    }
}

/*---------------------------------------------------------------
 * I2S write helper: 16-bit PCM → 32-bit I2S expansion + volume
 *-------------------------------------------------------------*/
static esp_err_t write_pcm_to_i2s(const int16_t *pcm_data, size_t sample_count)
{
    /* 使用预分配的静态缓冲区，避免每次TTS回调都malloc/free */
    if (sample_count > 960) {
        ESP_LOGW(TAG, "Sample count %d exceeds buffer size, truncating", (int)sample_count);
        sample_count = 960;
    }

    /* 音量缩放因子：0-100% 映射到 0-0x7FFF */
    int32_t vol_scale = (s_volume * 0x7FFF) / 100;

    /* Push volume-scaled 16-bit PCM to reference ring buffer for AEC.
     * This must be done BEFORE the 32-bit expansion overwrites the buffer. */
    if (s_ref_ringbuf != NULL) {
        int16_t ref_buf[960];
        for (size_t i = 0; i < sample_count; i++) {
            ref_buf[i] = (int16_t)((int32_t)pcm_data[i] * vol_scale / 0x7FFF);
        }
        xRingbufferSend(s_ref_ringbuf, ref_buf, sample_count * sizeof(int16_t), 0);
    }

    /* 16-bit有符号扩展到32-bit，左移16位对齐到32-bit I2S slot高有效位 */
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = (int32_t)pcm_data[i];
        sample = sample * vol_scale / 0x7FFF;
        s_pcm32_buf[i] = sample << 16;
    }

    size_t bytes_written = 0;
    size_t buf32_size = sample_count * sizeof(int32_t);
    esp_err_t ret = i2s_channel_write(s_tx_handle, s_pcm32_buf, buf32_size, &bytes_written, pdMS_TO_TICKS(500));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/*---------------------------------------------------------------
 * Decode task: OPUS decode + I2S playback
 *
 * This task runs in its own context with proper coprocessor (FPU/PIE/HWLOOP)
 * lazy context save areas allocated from its stack. The OPUS decoder's
 * PIE SIMD instructions (opus_dot16_32_opt etc.) execute here safely,
 * avoiding the MCAUSE 0x1f coprocessor exception that occurred when
 * they ran in the WebSocket client task context.
 *-------------------------------------------------------------*/
static void audio_decode_task(void *arg)
{
    ESP_LOGI(TAG, "Audio decode task started (stack=%d, priority=%d, core=%d)",
             DECODE_TASK_STACK_SIZE, DECODE_TASK_PRIORITY, DECODE_TASK_CORE);

    /* Explicitly enable PIE and FPU coprocessors for this task.
     *
     * On ESP32-P4 rev < v3, the EXT_ILL_CSR hardware bug can cause
     * PIE instructions to trigger MCAUSE=0x1f instead of the expected
     * MCAUSE=0x02 (Illegal Instruction). The FreeRTOS coprocessor
     * lazy context switch handler only recognizes MCAUSE=0x02, so
     * when MCAUSE=0x1f occurs, PIE is never enabled and the task
     * crashes with "Unknown" exception.
     *
     * Workaround: explicitly enable PIE and FPU at task startup,
     * bypassing the lazy mechanism. This ensures PIE SIMD instructions
     * (opus_dot16_32_opt, opus_silk_lpc_fir16_opt etc.) can execute
     * without triggering coprocessor exceptions.
     *
     * CSR_PIE_STATE_REG (0x7F2): 0=OFF, 1=Initial, 2=Clean, 3=Dirty
     * We set it to 1 (Initial) to enable PIE with fresh register state.
     */
#if SOC_CPU_HAS_PIE
    rv_utils_enable_pie();
    ESP_LOGI(TAG, "PIE coprocessor explicitly enabled (CSR 0x7F2 = %lu)",
             (unsigned long)RV_READ_CSR(CSR_PIE_STATE_REG));
#endif

#if SOC_CPU_HAS_FPU
    /* Also ensure FPU is enabled — OPUS uses FPU floating-point ops too */
    rv_utils_enable_fpu();
#endif

    s_decode_task_running = true;
    uint8_t *opus_data = NULL;

    while (s_decode_task_running) {
        /* Wait for OPUS frame data pointer from the queue */
        if (xQueueReceive(s_decode_queue, &opus_data, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        if (opus_data == NULL) {
            /* NULL pointer is the shutdown signal */
            break;
        }

        /* Re-enable PIE and FPU before each decode cycle.
         * FreeRTOS disables coprocessors during context switch (sets
         * mstatus.FS=Off and CSR_PIE_STATE_REG=0). If this task was
         * preempted and later resumed, both would be OFF, causing
         * MCAUSE=0x1f coprocessor exceptions on the next PIE/FPU instruction.
         * On ESP32-P4 rev < v3, the EXT_ILL_CSR bug causes coprocessor
         * exceptions to generate MCAUSE=0x1f instead of MCAUSE=0x02,
         * which the FreeRTOS lazy context switch handler cannot recognize.
         * Re-enable both here to bypass the broken lazy mechanism. */
#if SOC_CPU_HAS_PIE
        rv_utils_enable_pie();
#endif
#if SOC_CPU_HAS_FPU
        rv_utils_enable_fpu();
#endif

        /* Read the actual data length from the first 2 bytes */
        uint16_t data_len = (uint16_t)(opus_data[0] | (opus_data[1] << 8));
        uint8_t *frame_data = opus_data + 2;

        /* Lazy-create OPUS decoder on first frame */
        if (s_opus_decoder == NULL) {
            esp_err_t ret = opus_decoder_create();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create OPUS decoder, discarding %d bytes", data_len);
                free(opus_data);
                opus_data = NULL;
                continue;
            }
        }

        if (s_opus_out_buf == NULL) {
            ESP_LOGD(TAG, "OPUS output buffer not ready, discarding %d bytes", data_len);
            free(opus_data);
            opus_data = NULL;
            continue;
        }

        /* Decode OPUS frame → PCM (runs PIE SIMD instructions in this task context) */
        esp_audio_dec_in_raw_t raw = {
            .buffer       = frame_data,
            .len          = (uint32_t)data_len,
            .consumed     = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };

        esp_audio_dec_out_frame_t frame = {
            .buffer       = (uint8_t *)s_opus_out_buf,
            .len          = OPUS_DEC_OUT_BUF_SIZE,
            .needed_size  = 0,
            .decoded_size = 0,
        };

        esp_audio_dec_info_t dec_info = {0};

        esp_audio_err_t ret = esp_opus_dec_decode(s_opus_decoder, &raw, &frame, &dec_info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "OPUS decode failed: %d (len=%d)", ret, data_len);
            free(opus_data);
            opus_data = NULL;
            continue;
        }

        if (frame.decoded_size == 0) {
            ESP_LOGD(TAG, "OPUS decode produced 0 bytes");
            free(opus_data);
            opus_data = NULL;
            continue;
        }

        /* decoded_size是字节数，转换为sample数 */
        size_t sample_count = frame.decoded_size / sizeof(int16_t);

        ESP_LOGD(TAG, "OPUS decoded: %d bytes → %d samples (%dHz, %dch)",
                 frame.decoded_size, sample_count, dec_info.sample_rate, dec_info.channel);

        /* 将16-bit PCM写入I2S（扩展为32-bit + 音量控制） */
        write_pcm_to_i2s(s_opus_out_buf, sample_count);

        free(opus_data);
        opus_data = NULL;
    }

    ESP_LOGI(TAG, "Audio decode task exiting");
    s_decode_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t decode_task_start(void)
{
    if (s_decode_task_handle != NULL) {
        return ESP_OK;  /* Already running */
    }

    if (s_decode_queue == NULL) {
        s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(uint8_t *));
        if (s_decode_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create decode queue");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t core = (DECODE_TASK_CORE < 0) ? tskNO_AFFINITY : DECODE_TASK_CORE;

    memset(&s_decode_task_tcb, 0, sizeof(s_decode_task_tcb));
    s_decode_task_handle = xTaskCreateStaticPinnedToCore(
        audio_decode_task,
        DECODE_TASK_NAME,
        DECODE_TASK_STACK_SIZE,
        NULL,               /* task parameter */
        DECODE_TASK_PRIORITY,
        s_decode_task_stack,
        &s_decode_task_tcb,
        core
    );

    if (s_decode_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create decode task");
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Decode task created (stack=%d, prio=%d, core=%d)",
             DECODE_TASK_STACK_SIZE, DECODE_TASK_PRIORITY, core);
    return ESP_OK;
}

static void decode_task_stop(void)
{
    if (s_decode_task_handle == NULL) {
        return;
    }

    s_decode_task_running = false;

    /* Send shutdown signal (NULL pointer) */
    uint8_t *shutdown_sig = NULL;
    xQueueSend(s_decode_queue, &shutdown_sig, pdMS_TO_TICKS(500));

    /* Wait for task to exit */
    vTaskDelay(pdMS_TO_TICKS(200));

    s_decode_task_handle = NULL;

    if (s_decode_queue != NULL) {
        /* Drain any remaining items and free their buffers */
        uint8_t *item = NULL;
        while (xQueueReceive(s_decode_queue, &item, 0) == pdTRUE) {
            if (item != NULL) {
                free(item);
            }
        }
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
    }
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t audio_bridge_init(const audio_bridge_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_volume = CONFIG_AUDIO_BRIDGE_DEFAULT_VOLUME;

    // Step 1: Create channel pair (full-duplex)
    /* DMA缓冲区配置（对齐 mipi_dsi 参考项目）：
     * dma_desc_num=16: 消息队列深度=15，避免 uxQueueSpacesAvailable<=1
     * 强制缓冲区切换导致 partial read 超时（原 desc_num=12 队列深度=11 不足）
     * dma_frame_num=960: OPUS 60ms帧 = 960 samples × 4 bytes = 3840 bytes/帧
     * 16×3840 = 61440 bytes 总缓冲，~960ms 缓冲深度 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 16;
    chan_cfg.dma_frame_num = 960;
    chan_cfg.auto_clear    = true;

    ESP_LOGI(TAG, "Creating I2S channels: port=0, dma_desc=%d, dma_frame=%d",
             chan_cfg.dma_desc_num, chan_cfg.dma_frame_num);

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel pair: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S channel pair created (TX=%p, RX=%p)", s_tx_handle, s_rx_handle);

    // Step 2: Configure TX and RX with the SAME std_cfg (full-duplex requirement)
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = config->bclk_gpio,
                .ws   = config->ws_gpio,
                .dout = config->dout_gpio,
                .din  = config->din_gpio,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_LOGI(TAG, "Config: PHILIPS, MONO, LEFT, %dHz, 32-bit", config->sample_rate);
    ESP_LOGI(TAG, "GPIO: BCLK=%d, WS=%d, DOUT=%d, DIN=%d",
             config->bclk_gpio, config->ws_gpio, config->dout_gpio, config->din_gpio);

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(ret));
        goto cleanup_channels;
    }
    ESP_LOGI(TAG, "TX channel initialized");

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(ret));
        goto cleanup_channels;
    }
    ESP_LOGI(TAG, "RX channel initialized");

    // Step 3: Preload TX DMA buffers before enabling
    int32_t preload_buf[64] = {0};
    size_t preload_bytes    = sizeof(preload_buf);
    int preload_count       = 0;
    while (preload_bytes == sizeof(preload_buf)) {
        ret = i2s_channel_preload_data(s_tx_handle, preload_buf, sizeof(preload_buf), &preload_bytes);
        if (ret != ESP_OK) {
            break;
        }
        preload_count++;
    }
    ESP_LOGI(TAG, "Preloaded %d TX DMA buffers", preload_count);

    // Step 4: Enable TX only (RX is enabled on-demand when mic starts)
    // Deferred RX enable avoids DMA stalling when no consumer reads for extended
    // periods. See mipi_dsi reference: enable + delay + test read pattern.
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(ret));
        goto cleanup_channels;
    }

    // Step 5: Start decode task
    ret = decode_task_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start decode task");
        goto cleanup_channels;
    }

    // Step 6: Create reference signal ring buffer for AEC
    // Use RINGBUF_TYPE_BYTEBUF (not NOSPLIT) because xRingbufferReceiveUpTo()
    // only supports BYTEBUF and RINGBUF types. PCM ref data is a byte stream,
    // so BYTEBUF is the correct choice (allows partial reads via ReceiveUpTo).
    s_ref_ringbuf = xRingbufferCreate(REF_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_ref_ringbuf == NULL) {
        ESP_LOGW(TAG, "Failed to create AEC ref ringbuf — AEC will be unavailable");
    } else {
        ESP_LOGI(TAG, "AEC reference ringbuf created (%d bytes)", REF_RINGBUF_SIZE);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio bridge initialized successfully (dedicated decode task)");
    return ESP_OK;

cleanup_channels:
    i2s_del_channel(s_tx_handle);
    i2s_del_channel(s_rx_handle);
    s_tx_handle = NULL;
    s_rx_handle = NULL;
    return ret;
}

esp_err_t audio_bridge_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_OK;
    }

    decode_task_stop();
    opus_decoder_destroy();

    /* Disable RX channel if it was enabled (mic may have been running) */
    if (s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
    }

    // i2s_del_channel will automatically disable the channel if enabled
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }

    if (s_rx_handle) {
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }

    if (s_ref_ringbuf != NULL) {
        vRingbufferDelete(s_ref_ringbuf);
        s_ref_ringbuf = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Audio bridge deinitialized");
    return ESP_OK;
}

esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_initialized || s_tx_handle == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || bytes_written == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(s_tx_handle, data, len, bytes_written, ticks_to_wait);
}

esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_initialized || s_rx_handle == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || bytes_read == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks_to_wait = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_read(s_rx_handle, data, len, bytes_read, ticks_to_wait);
}

void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "TTS callback but not initialized, discarding %d bytes", len);
        return;
    }

    if (data == NULL || len <= 0) {
        return;
    }

    if (s_decode_queue == NULL) {
        ESP_LOGW(TAG, "Decode queue not ready, discarding %d bytes", len);
        return;
    }

    /* Allocate buffer and copy OPUS data.
     * This runs in the WebSocket client task context (lightweight — no PIE/FPU here).
     * Format: [2 bytes data_len (little-endian)] [data_len bytes OPUS frame data]
     * The decode task will read the length prefix and decode in its own context. */
    size_t buf_size = 2 + (size_t)len;
    uint8_t *opus_buf = malloc(buf_size);
    if (opus_buf == NULL) {
        ESP_LOGW(TAG, "Failed to allocate %d bytes for OPUS frame, discarding", (int)buf_size);
        return;
    }

    /* Write length prefix */
    opus_buf[0] = (uint8_t)(len & 0xFF);
    opus_buf[1] = (uint8_t)((len >> 8) & 0xFF);

    /* Copy OPUS frame data */
    memcpy(opus_buf + 2, data, (size_t)len);

    /* Send buffer pointer to decode task queue.
     * Use 0 timeout to avoid blocking the WebSocket task.
     * If queue is full, drop the oldest frame to keep latency low. */
    if (xQueueSend(s_decode_queue, &opus_buf, 0) != pdTRUE) {
        /* Queue full — drop oldest frame and retry */
        uint8_t *old_buf = NULL;
        if (xQueueReceive(s_decode_queue, &old_buf, 0) == pdTRUE && old_buf != NULL) {
            free(old_buf);
        }
        if (xQueueSend(s_decode_queue, &opus_buf, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Decode queue full, dropping OPUS frame (%d bytes)", len);
            free(opus_buf);
        }
    }
}

esp_err_t audio_bridge_set_volume(int volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Invalid volume: %d (must be 0-100)", volume_percent);
        return ESP_ERR_INVALID_ARG;
    }

    s_volume = volume_percent;
    ESP_LOGI(TAG, "Volume set to %d%%", s_volume);
    return ESP_OK;
}

int audio_bridge_get_volume(void)
{
    return s_volume;
}

/*---------------------------------------------------------------
 * Microphone capture + OPUS encoding
 *-------------------------------------------------------------*/
static TaskHandle_t s_mic_task_handle = NULL;
static volatile bool s_mic_task_running = false;
static audio_bridge_mic_callback_t s_mic_callback = NULL;
static void *s_mic_callback_ctx = NULL;

/* Raw PCM callback for wake word engine (ESP-SR AFE/MultiNet) */
static audio_bridge_pcm_callback_t s_pcm_callback = NULL;
static void *s_pcm_callback_ctx = NULL;

/* OPUS encoder state (created on first mic_start) */
static void *s_opus_encoder = NULL;

/* Mic task configuration */
#define MIC_TASK_STACK_SIZE    CONFIG_AUDIO_BRIDGE_MIC_TASK_STACK_SIZE
#define MIC_TASK_PRIORITY      CONFIG_AUDIO_BRIDGE_DECODE_TASK_PRIORITY
#define MIC_TASK_CORE          CONFIG_AUDIO_BRIDGE_DECODE_TASK_CORE
#define MIC_TASK_NAME          "audio_mic"

/* OPUS 60ms frame at 16kHz mono = 960 samples.
 * I2S 32-bit → need to downsample to 16-bit for OPUS encoder.
 * Read 960 * 4 = 3840 bytes from I2S RX per frame. */
#define MIC_FRAME_SAMPLES      960
#define MIC_I2S_READ_SIZE      (MIC_FRAME_SAMPLES * sizeof(int32_t))
#define MIC_PCM16_BUF_SIZE     (MIC_FRAME_SAMPLES * sizeof(int16_t))
#define MIC_OPUS_OUT_BUF_SIZE  1500

static StaticTask_t s_mic_task_tcb;
static StackType_t s_mic_task_stack[MIC_TASK_STACK_SIZE];

static esp_err_t opus_encoder_create(void)
{
    if (s_opus_encoder != NULL) {
        return ESP_OK;
    }

    /* OPUS encoder memory allocation diagnostics.
     * Previous code forced all malloc to internal RAM via
     * heap_caps_malloc_extmem_enable((size_t)-1), which caused
     * OPUS_ALLOC_FAIL (-7) when internal RAM was insufficient
     * (AFE task + wake word engine already consume significant internal RAM).
     * Now use default allocation strategy (PSRAM-preferred), aligned with
     * official esp_xiaozhi audio_service.h which does NOT force internal RAM.
     * OPUS encoder does not use PIE SIMD on its working data (unlike decoder),
     * so PSRAM allocation is safe here. */
    ESP_LOGI(TAG, "OPUS encoder create: internal free=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_opus_enc_config_t cfg = {
        .sample_rate      = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel          = ESP_AUDIO_MONO,
        .bits_per_sample  = ESP_AUDIO_BIT16,
        .bitrate          = ESP_OPUS_BITRATE_AUTO,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 0,
        .enable_fec       = false,
        .enable_dtx       = true,
        .enable_vbr       = true,
    };

    esp_audio_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus_encoder);

    if (ret != ESP_AUDIO_ERR_OK || s_opus_encoder == NULL) {
        ESP_LOGE(TAG, "Failed to open OPUS encoder: %d (internal free=%u, PSRAM free=%u)",
                 ret,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_opus_encoder = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OPUS encoder created (16kHz, mono, 60ms, VOIP, complexity=0)");
    return ESP_OK;
}

static void opus_encoder_destroy(void)
{
    if (s_opus_encoder != NULL) {
        esp_opus_enc_close(s_opus_encoder);
        s_opus_encoder = NULL;
    }
}

static void mic_task(void *arg)
{
    ESP_LOGI(TAG, "Mic task started");

    /* Enable FPU for this task (OPUS encoder uses FPU) */
#if SOC_CPU_HAS_FPU
    rv_utils_enable_fpu();
#endif

    s_mic_task_running = true;

    /* Create OPUS encoder */
    if (opus_encoder_create() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OPUS encoder, mic task exiting");
        s_mic_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Allocate buffers in internal DRAM */
    int32_t *i2s_rx_buf = heap_caps_malloc(MIC_I2S_READ_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm16_buf = heap_caps_malloc(MIC_PCM16_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *opus_out_buf = heap_caps_malloc(MIC_OPUS_OUT_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (i2s_rx_buf == NULL || pcm16_buf == NULL || opus_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mic buffers");
        goto mic_cleanup;
    }

    /* Get encoder frame info */
    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(s_opus_encoder, &in_size, &out_size);
    ESP_LOGI(TAG, "OPUS encoder: in_size=%d, out_size=%d", in_size, out_size);

    int frame_count = 0;

    while (s_mic_task_running) {
        /* Read one 60ms frame from I2S RX (960 samples * 4 bytes = 3840 bytes)
         * Timeout 1000ms (aligned with mipi_dsi reference: recorder uses 1000ms).
         * Shorter timeouts (200ms) caused ESP_ERR_TIMEOUT after DMA queue pressure
         * forced buffer switches (partial reads). */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle, i2s_rx_buf, MIC_I2S_READ_SIZE,
                                          &bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S RX read failed: %s (bytes_read=%u)", esp_err_to_name(ret), (unsigned)bytes_read);
            continue;
        }

        if (bytes_read < MIC_I2S_READ_SIZE) {
            /* Partial read — log and skip (should be rare with dma_desc_num=16) */
            ESP_LOGW(TAG, "I2S RX partial read: %u/%u bytes", (unsigned)bytes_read, (unsigned)MIC_I2S_READ_SIZE);
            continue;
        }

        /* Downsample 32-bit I2S to 16-bit PCM for OPUS encoder */
        for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
            /* I2S data is left-shifted by 16 bits, right-shift to get 16-bit */
            pcm16_buf[i] = (int16_t)(i2s_rx_buf[i] >> 16);
        }

        /* Deliver raw PCM to wake word engine if registered */
        if (s_pcm_callback != NULL) {
            s_pcm_callback(pcm16_buf, MIC_FRAME_SAMPLES, s_pcm_callback_ctx);
        }

        /* Debug: log first few raw I2S samples and PCM values every 50 frames */
        frame_count++;
        if (frame_count <= 3 || (frame_count % 50 == 0)) {
            int32_t max_val = 0;
            for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
                int32_t abs_val = pcm16_buf[i] > 0 ? pcm16_buf[i] : -pcm16_buf[i];
                if (abs_val > max_val) max_val = abs_val;
            }
            ESP_LOGI(TAG, "Mic frame #%d: i2s[0..3]=%08X,%08X,%08X,%08X pcm[0..3]=%d,%d,%d,%d peak=%ld",
                     frame_count,
                     (unsigned)i2s_rx_buf[0], (unsigned)i2s_rx_buf[1],
                     (unsigned)i2s_rx_buf[2], (unsigned)i2s_rx_buf[3],
                     pcm16_buf[0], pcm16_buf[1], pcm16_buf[2], pcm16_buf[3],
                     (long)max_val);
        }

        /* Encode to OPUS */
        esp_audio_enc_in_frame_t in_frame = {
            .buffer = (uint8_t *)pcm16_buf,
            .len = MIC_PCM16_BUF_SIZE,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = opus_out_buf,
            .len = MIC_OPUS_OUT_BUF_SIZE,
        };

        esp_audio_err_t enc_ret = esp_opus_enc_process(s_opus_encoder, &in_frame, &out_frame);
        if (enc_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "OPUS encode failed: %d", enc_ret);
            continue;
        }

        /* Get encoded output size from out_frame.encoded_bytes (NOT .len — .len is the input buffer size) */
        int encoded_len = (int)out_frame.encoded_bytes;
        if (encoded_len <= 0) {
            continue;
        }

        /* Deliver OPUS frame via callback */
        if (s_mic_callback != NULL) {
            s_mic_callback(opus_out_buf, encoded_len, s_mic_callback_ctx);
        }
    }

mic_cleanup:
    if (i2s_rx_buf) heap_caps_free(i2s_rx_buf);
    if (pcm16_buf) heap_caps_free(pcm16_buf);
    if (opus_out_buf) heap_caps_free(opus_out_buf);

    opus_encoder_destroy();

    ESP_LOGI(TAG, "Mic task exiting");
    s_mic_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_bridge_mic_start(audio_bridge_mic_callback_t callback, void *ctx)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (callback == NULL) {
        ESP_LOGE(TAG, "Callback is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mic_task_handle != NULL) {
        ESP_LOGW(TAG, "Mic already running");
        return ESP_OK;
    }

    /* Enable RX channel on-demand (deferred from init).
     * i2s_channel_enable resets the RX message queue, ensuring a clean DMA state.
     * Add a short delay after enable and do a test read to verify RX is working,
     * following the same pattern as the mipi_dsi reference project. */
    esp_err_t ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Test read to verify RX DMA is producing data */
    int32_t test_buf[64];
    size_t test_bytes = 0;
    for (int i = 0; i < 3; i++) {
        ret = i2s_channel_read(s_rx_handle, test_buf, sizeof(test_buf), &test_bytes, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "RX test read %d: %s (%u bytes)", i + 1, esp_err_to_name(ret), (unsigned)test_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "RX test read %d failed, continuing anyway", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_mic_callback = callback;
    s_mic_callback_ctx = ctx;
    s_mic_task_running = true;

    BaseType_t core = (MIC_TASK_CORE < 0) ? tskNO_AFFINITY : MIC_TASK_CORE;
    memset(&s_mic_task_tcb, 0, sizeof(s_mic_task_tcb));

    s_mic_task_handle = xTaskCreateStaticPinnedToCore(
        mic_task,
        MIC_TASK_NAME,
        MIC_TASK_STACK_SIZE,
        NULL,
        MIC_TASK_PRIORITY,
        s_mic_task_stack,
        &s_mic_task_tcb,
        core
    );

    if (s_mic_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create mic task");
        s_mic_callback = NULL;
        s_mic_callback_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Mic task started");
    return ESP_OK;
}

esp_err_t audio_bridge_mic_stop(void)
{
    if (s_mic_task_handle == NULL) {
        return ESP_OK;
    }

    s_mic_task_running = false;

    /* Step 1: Disable RX channel FIRST to unblock any pending i2s_channel_read().
     * When the RX channel is disabled, pending reads return immediately with
     * ESP_ERR_INVALID_STATE, allowing the task to check s_mic_task_running
     * and exit its main loop. The previous order (delay -> NULL handle -> disable)
     * left the task blocked in i2s_channel_read for up to 1000ms, then
     * mic_start reused the StaticTask's TCB/stack while the old task was
     * still running, corrupting AFE ringbuffer state. */
    if (s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
    }

    /* Step 2: Wait for task to actually exit (up to 2000ms).
     * mic_task sets s_mic_task_handle = NULL in its cleanup path
     * before calling vTaskDelete(NULL). Polling this variable guarantees
     * we don't return before the task has fully exited, which would
     * let mic_start overwrite the StaticTask's memory while the old
     * task is still running. */
    int wait_ms = 0;
    while (s_mic_task_handle != NULL && wait_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }

    if (s_mic_task_handle != NULL) {
        /* Task did not exit gracefully -- force delete as last resort.
         * For StaticTask, vTaskDelete only removes the task from the
         * scheduler lists; it does not free the static TCB/stack.
         * Safe but may leak resources if the task was holding locks. */
        ESP_LOGW(TAG, "Mic task did not exit in 2000ms, force deleting");
        TaskHandle_t handle = s_mic_task_handle;
        s_mic_task_handle = NULL;
        vTaskDelete(handle);
    }

    s_mic_callback = NULL;
    s_mic_callback_ctx = NULL;

    ESP_LOGI(TAG, "Mic task stopped");
    return ESP_OK;
}

void audio_bridge_register_pcm_callback(audio_bridge_pcm_callback_t callback, void *ctx)
{
    s_pcm_callback = callback;
    s_pcm_callback_ctx = ctx;
    if (callback) {
        ESP_LOGI(TAG, "PCM callback registered for wake word engine");
    } else {
        ESP_LOGI(TAG, "PCM callback unregistered");
    }
}

int audio_bridge_read_ref_pcm(int16_t *out_buf, int samples, uint32_t timeout_ms)
{
    if (out_buf == NULL || samples <= 0) {
        return -1;
    }
    if (s_ref_ringbuf == NULL) {
        memset(out_buf, 0, samples * sizeof(int16_t));
        return 0;
    }

    size_t needed = samples * sizeof(int16_t);
    size_t item_size = 0;
    char *item = (char *)xRingbufferReceiveUpTo(s_ref_ringbuf, &item_size, pdMS_TO_TICKS(timeout_ms), needed);

    if (item != NULL && item_size > 0) {
        int samples_read = item_size / sizeof(int16_t);
        memcpy(out_buf, item, item_size);
        vRingbufferReturnItem(s_ref_ringbuf, item);
        /* Zero-fill remaining if we got less than requested */
        if (samples_read < samples) {
            memset(&out_buf[samples_read], 0, (samples - samples_read) * sizeof(int16_t));
        }
        return samples_read;
    }

    /* No reference data available — silence (no TTS playing) */
    memset(out_buf, 0, needed);
    return 0;
}

#else /* AUDIO_BRIDGE_ENABLE == 0 */

esp_err_t audio_bridge_init(const audio_bridge_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
    (void)bytes_written;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
    (void)bytes_read;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
}

esp_err_t audio_bridge_set_volume(int volume_percent)
{
    (void)volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

int audio_bridge_get_volume(void)
{
    return 0;
}

esp_err_t audio_bridge_mic_start(audio_bridge_mic_callback_t callback, void *ctx)
{
    (void)callback;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bridge_mic_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_bridge_register_pcm_callback(audio_bridge_pcm_callback_t callback, void *ctx)
{
    (void)callback;
    (void)ctx;
}

int audio_bridge_read_ref_pcm(int16_t *out_buf, int samples, uint32_t timeout_ms)
{
    (void)out_buf;
    (void)samples;
    (void)timeout_ms;
    return -1;
}

#endif /* AUDIO_BRIDGE_ENABLE */
