/*
 * Smart Voice Assistant for ESP32-P4 + C5 (WT01P4C5-S1)
 *
 * Integrates WiFi Manager, WebSocket Manager, and Xiaozhi Voice Assistant.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "connection_manager.h"
#include "heartbeat_service.h"
#include "message_queue.h"
#include "wifi_manager.h"
#include "websocket_manager.h"
#include "xiaozhi_manager.h"
#include "audio_bridge.h"
#include "wake_word_engine.h"
#include "focuslamp_bridge.h"
#include "status_receiver.h"
#include "tts_http_api.h"
#include "device_info_api.h"
#include "focuslamp_discovery.h"
#include "device_state.h"
#include "event_bus.h"
#include "led_service.h"
#include "servo_driver.h"
#include "motion_controller.h"
#include "ambient_service.h"
#include "peripheral_inputs.h"
#include "controller_modes.h"
#include "mode_http_api.h"
#include "lcd_driver.h"
#include "lcd_status_ui.h"
#include "demo_behaviors.h"
#include "sdkconfig.h"

static const char *TAG = "MAIN";

/* wifi_manager_init() already waits up to 30 seconds for the initial
 * connection. Keep the total startup WiFi budget near 60 seconds. */
#define WIFI_STARTUP_WAIT_TIMEOUT_MS 30000
#define WIFI_STARTUP_WAIT_LOG_INTERVAL_MS 2000

/* Forward declaration: async chat state reporter (defined below) */
static void chat_state_report(bool active);

/*---------------------------------------------------------------
 * WiFi event callback
 *-------------------------------------------------------------*/
static void wifi_event_callback(wifi_manager_event_t event, void *data)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WiFi] Connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WiFi] Disconnected");
        break;
    case WIFI_MANAGER_EVENT_GOT_IP: {
        wifi_manager_info_t info;
        if (wifi_manager_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "[WiFi] Got IP: %s", info.ip);
        }
        break;
    }
    case WIFI_MANAGER_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "[WiFi] Scan done");
        break;
    case WIFI_MANAGER_EVENT_RECONNECTING:
        ESP_LOGI(TAG, "[WiFi] Reconnecting with backoff");
        break;
    case WIFI_MANAGER_EVENT_HOSTED_TIMEOUT:
        ESP_LOGW(TAG, "[WiFi] ESP-Hosted heartbeat timeout");
        break;
    }
    focuslamp_discovery_on_wifi_event(event, data);
}

/*---------------------------------------------------------------
 * WebSocket event callback
 *-------------------------------------------------------------*/
static void ws_data_callback(ws_manager_event_t event, void *data)
{
    switch (event) {
    case WS_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WS] Connected");
        break;
    case WS_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WS] Disconnected");
        break;
    case WS_MANAGER_EVENT_DATA: {
        ws_manager_data_t *msg = (ws_manager_data_t *)data;
        if (msg && msg->data_len > 0 && msg->data_len < 256) {
            ESP_LOGI(TAG, "[WS] Data: type=%s len=%d",
                     msg->type == WS_DATA_TYPE_TEXT ? "TEXT" : "BIN",
                     msg->data_len);
        }
        break;
    }
    case WS_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[WS] Error");
        break;
    case WS_MANAGER_EVENT_SERVER_CONNECT:
        ESP_LOGI(TAG, "[WS] Server client connected");
        break;
    case WS_MANAGER_EVENT_SERVER_DISCONNECT:
        ESP_LOGI(TAG, "[WS] Server client disconnected");
        break;
    case WS_MANAGER_EVENT_CLIENT_TIMEOUT:
        ESP_LOGW(TAG, "[WS] Server client heartbeat timeout");
        break;
    }
}

/*---------------------------------------------------------------
 * Wake dispatch: never wait for network/server hello in the AFE task.
 * One outstanding request covers both queued and executing states.
 * Repeated detections while busy are ignored, not replayed after a timeout.
 * app_main owns this worker for the lifetime of the application.
 *-------------------------------------------------------------*/
#define WAKE_DISPATCH_STACK_BYTES 4096
#define WAKE_DISPATCH_PRIORITY 4
typedef struct {
    char phrase[256];
} wake_request_t;

static QueueHandle_t s_wake_queue;
static portMUX_TYPE s_wake_dispatch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_wake_outstanding;

static void wake_dispatch_task(void *arg)
{
    (void)arg;
    wake_request_t request;
    for (;;) {
        if (xQueueReceive(s_wake_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        /* No dispatch lock is held across this potentially 10 s operation. */
        esp_err_t ret = xiaozhi_manager_send_wake_word(request.phrase);
        portENTER_CRITICAL(&s_wake_dispatch_lock);
        s_wake_outstanding = false;
        portEXIT_CRITICAL(&s_wake_dispatch_lock);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[WakeWord] Async trigger failed: %s; wake again after recovery",
                     esp_err_to_name(ret));
        }
    }
}

static esp_err_t wake_dispatch_init(void)
{
    const uint32_t caps = MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL;
    ESP_LOGI(TAG, "Wake dispatch memory [before]: free=%u largest=%u caps=0x%x",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps), (unsigned)caps);
    s_wake_queue = xQueueCreate(1, sizeof(wake_request_t));
    if (!s_wake_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(wake_dispatch_task, "wake_dispatch", WAKE_DISPATCH_STACK_BYTES,
                    NULL, WAKE_DISPATCH_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_wake_queue);
        s_wake_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Wake dispatcher ready: stack=4096 priority=4, duplicates ignored while pending/busy");
    ESP_LOGI(TAG, "Wake dispatch memory [after]: free=%u largest=%u caps=0x%x",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps), (unsigned)caps);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Wake word detection callback
 * Called by wake_word_engine when a custom wake word is detected.
 * Triggers xiaozhi_manager to start a conversation.
 *-------------------------------------------------------------*/
static void wake_word_detect_callback(int command_id, const char *command_str,
                                      wake_word_lang_t lang, float prob, void *ctx)
{
    ESP_LOGI(TAG, "[WakeWord] Detected: id=%d, cmd='%s', lang=%s, prob=%.2f",
             command_id, command_str ? command_str : "null",
             lang == WAKE_WORD_LANG_CN ? "CN" : "EN", prob);

    if (!s_wake_queue) {
        ESP_LOGE(TAG, "[WakeWord] Dispatcher unavailable; no synchronous fallback");
        return;
    }
    wake_request_t request;
    /* The callback's source string expires on return; queue owns a copy. */
    snprintf(request.phrase, sizeof(request.phrase), "%s",
             command_str ? command_str : "你好小智");
    portENTER_CRITICAL(&s_wake_dispatch_lock);
    bool busy = s_wake_outstanding;
    if (!busy) s_wake_outstanding = true;
    portEXIT_CRITICAL(&s_wake_dispatch_lock);
    if (busy) {
        ESP_LOGI(TAG, "[WakeWord] Duplicate ignored: request pending/in progress");
        return;
    }
    if (xQueueSend(s_wake_queue, &request, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_wake_dispatch_lock);
        s_wake_outstanding = false;
        portEXIT_CRITICAL(&s_wake_dispatch_lock);
        ESP_LOGE(TAG, "[WakeWord] Async dispatch failed; no synchronous fallback");
    }
}

/*---------------------------------------------------------------
 * Barge-in (voice interrupt) — 后续测试项（暂未启用）
 *
 * wake_word_engine 已实现 VAD 打断（BARGE_IN_PEAK_THRESHOLD /
 * BARGE_IN_FRAME_COUNT 可调），启用方式：
 *   1. xiaozhi_manager TTS_START/STOP 调用 wake_word_engine_set_tts_active()
 *   2. 本文件 wake_cfg.barge_in_cb = barge_in_callback;
 * 需实测调参（阈值/回声误触发），确认后恢复。
 *-------------------------------------------------------------*/

/*---------------------------------------------------------------
 * Xiaozhi event callback
 *-------------------------------------------------------------*/
static void xiaozhi_event_callback(xiaozhi_manager_event_t event, void *data, void *ctx)
{
    switch (event) {
    case XIAOZHI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[Xiaozhi] Connected to server — waiting for wake word");
        /* No longer auto-opens audio channel or sends wake word here.
         * Wake word is triggered only by ESP-SR detection or button press,
         * via xiaozhi_manager_send_wake_word(). */
        break;
    case XIAOZHI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[Xiaozhi] Disconnected from server");
        chat_state_report(false); /* A disconnected dialogue must not freeze focus. */
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel opened");
        /* No longer auto-sends wake word here.
         * The wake word should already have been sent by the trigger source
         * (ESP-SR or button) that opened the audio channel. */
        /* Update local dialogue state without HTTP so focus countdown pauses. */
        chat_state_report(true);
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel closed — back to idle");
        /* A closed channel ends dialogue; TTS_STOP alone still means listening. */
        chat_state_report(false);
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_START:
        ESP_LOGI(TAG, "[Xiaozhi] TTS started");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_STOP:
        ESP_LOGI(TAG, "[Xiaozhi] TTS stopped — continuing conversation (listening)");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_SENTENCE:
        ESP_LOGI(TAG, "[Xiaozhi] TTS sentence: %s", data ? (const char *)data : "");
        demo_behaviors_note_tts_text(data ? (const char *)data : NULL);
        break;
    case XIAOZHI_MANAGER_EVENT_STT_TEXT:
        ESP_LOGI(TAG, "[Xiaozhi] STT text: %s", data ? (const char *)data : "");
        break;
    case XIAOZHI_MANAGER_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "[Xiaozhi] Server goodbye — conversation ended, waiting for wake word");
        break;
    case XIAOZHI_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[Xiaozhi] Error occurred");
        break;
    }
}

/*---------------------------------------------------------------
 * Async local chat state storage
 *
 * The mode component is the single, short-lock source of voice activity.
 * Keep queue/task logging asynchronous; a full log queue must not lose a
 * dialogue-end update and leave the focus countdown paused forever.
 *-------------------------------------------------------------*/
#define CHAT_STATE_QUEUE_LEN 1

typedef struct {
    bool active;
} chat_state_report_t;

static QueueHandle_t s_chat_state_queue = NULL;

bool chat_state_is_active(void)
{
    return controller_modes_voice_active();
}

static void chat_state_task(void *arg)
{
    (void)arg;
    chat_state_report_t rpt;
    while (s_chat_state_queue) {
        if (xQueueReceive(s_chat_state_queue, &rpt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "[ChatState] active=%d (local, no HTTP)", rpt.active);
    }
    vTaskDelete(NULL);
}

static void chat_state_report(bool active)
{
    /* Only a bounded state update here: no allocation, HTTP or peripheral I/O.
     * This also preserves updates before the mode timer has been initialized. */
    controller_modes_set_voice_active(active);
    if (!s_chat_state_queue) {
        return;
    }
    chat_state_report_t rpt = {.active = active};
    if (xQueueOverwrite(s_chat_state_queue, &rpt) != pdTRUE) {
        ESP_LOGW(TAG, "[ChatState] log update failed active=%d (local state already set)", active);
    }
}

/*---------------------------------------------------------------
 * Xiaozhi audio callback (TTS audio data from server)
 * Routes OPUS-encoded TTS audio to I2S speaker via audio_bridge.
 * When audio hardware is available, audio_bridge_tts_callback
 * decodes OPUS and writes PCM to I2S TX channel.
 *-------------------------------------------------------------*/
static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx)
{
    audio_bridge_tts_callback(data, len, ctx);
}

/* Local strip setup is independent of network and voice startup. */
static void init_local_led(void)
{
    esp_err_t ret = device_state_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Local LED state init failed: %s (voice continues)", esp_err_to_name(ret));
        return;
    }

    int event_ret = event_bus_init();
    if (event_ret != 0) {
        ESP_LOGW(TAG, "Local LED event bus init failed: %d (voice continues)", event_ret);
        return;
    }

    ret = led_service_init();
    if (ret == ESP_OK) {
        ret = led_service_start();
        if (ret != ESP_OK) {
            esp_err_t cleanup_ret = led_service_deinit();
            if (cleanup_ret != ESP_OK) {
                ESP_LOGW(TAG, "Local LED cleanup failed: %s", esp_err_to_name(cleanup_ret));
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Local LED setup failed: %s (voice continues)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Local WS2812 strip ready (self.focuslamp.led.*)");
    }
}

/*---------------------------------------------------------------
 * Application entry point
 *-------------------------------------------------------------*/
/* Independent peripheral setup: no movement at boot, no BSP-wide GPIO init. */
static void init_local_motion(void)
{
    esp_err_t ret=servo_driver_init();
    const servo_em3_config_t em3={.uart_num=UART_NUM_1,.tx_pin=-1,.rx_pin=-1,.baud_rate=115200};
    const servo_lx_config_t lx={.uart_num=UART_NUM_2,.tx_pin=-1,.rx_pin=-1,.baud_rate=115200};
    if (ret==ESP_OK) ret=servo_em3_init(&em3);
    if (ret==ESP_OK) ret=servo_lx_init(&lx);
    if (ret==ESP_OK) ret=motion_controller_init();
    if (ret!=ESP_OK) ESP_LOGW(TAG,"Local arm init failed: %s (voice/light continue)",esp_err_to_name(ret));
    else {
        ESP_LOGI(TAG,"Local arm ready: self.arm.* and self.focuslamp.arm/servo.*");
        motion_log_positions();
    }
}
static void init_local_light(void)
{
    esp_err_t ret=ambient_service_start();
    if (ret!=ESP_OK) ESP_LOGW(TAG,"Local ambient init failed: %s (voice/arm continue)",esp_err_to_name(ret));
    else ESP_LOGI(TAG,"Local ambient sampling started (GPIO21, estimated lux, 10s freshness)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Voice Assistant for WT01P4C5-S1 ===");

    /* [FIX DIAG] Temporarily restore MCP logs to INFO to diagnose MCP
     * session initialization handshake (was WARN due to TTS stutter).
     * Keep the rest of the log level policy unchanged. */
    esp_log_level_set("ESP_XIAOZHI_CHAT", ESP_LOG_INFO);
    esp_log_level_set("esp_mcp_mgr", ESP_LOG_INFO);
    esp_log_level_set("ESP_XIAOZHI_MCP", ESP_LOG_INFO);
    /* Keep AFE errors, but silence its repeated empty-ringbuffer warnings.
     * This only changes logging; it does not stop microphone/AFE processing
     * or fix a failed cloud connection. Other tags keep their current level. */
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    init_local_led();
    init_local_motion();
    init_local_light();

    focuslamp_discovery_config_t discovery_cfg = {
        .self_role = FOCUSLAMP_ROLE_CONTROLLER,
        .kit_id = CONFIG_FOCUSLAMP_KIT_ID,
        .http_port = CONFIG_WS_MANAGER_SERVER_PORT,
    };
    esp_err_t discovery_ret = focuslamp_discovery_init(&discovery_cfg);
    bool discovery_initialized = discovery_ret == ESP_OK;
    if (!discovery_initialized) {
        ESP_LOGW(TAG, "FocusLamp discovery init failed: %s", esp_err_to_name(discovery_ret));
    }

    /* Step 1: Initialize WiFi Manager */
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_callback);

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Manager init failed: %s", esp_err_to_name(err));
        return;
    }

    /* wifi_manager_init() performs the first 30-second wait. Wait for one
     * additional bounded window, then continue in offline mode while the
     * WiFi manager keeps retrying in the background. */
    uint32_t wifi_waited_ms = 0;
    while (!wifi_manager_is_connected() && wifi_waited_ms < WIFI_STARTUP_WAIT_TIMEOUT_MS) {
        ESP_LOGI(TAG, "Waiting for WiFi connection... (%" PRIu32 "/%d ms)",
                 wifi_waited_ms, WIFI_STARTUP_WAIT_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_STARTUP_WAIT_LOG_INTERVAL_MS));
        wifi_waited_ms += WIFI_STARTUP_WAIT_LOG_INTERVAL_MS;
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi unavailable after startup timeout; continuing in offline mode");
    }

    /* Step 2: Initialize WebSocket Manager (independent of xiaozhi) */
    ws_manager_register_handler(WS_MANAGER_EVENT_CONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DISCONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DATA, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_ERROR, ws_data_callback);

    err = ws_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket Manager init failed: %s", esp_err_to_name(err));
    }

#if (WS_MANAGER_SERVER_ENABLE == 1)
    err = ws_manager_server_start();
    if (err == ESP_OK) {
        wifi_manager_info_t wifi_info;
        if (wifi_manager_get_info(&wifi_info) == ESP_OK) {
            ESP_LOGI(TAG, "WebSocket server running at ws://%s:%d/ws",
                     wifi_info.ip, WS_MANAGER_SERVER_PORT);
        }
        /* Initialize status receiver on the same HTTP server */
        err = status_receiver_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Status receiver initialized (POST /api/status/report)");
        } else {
            ESP_LOGW(TAG, "Status receiver init failed: %s", esp_err_to_name(err));
        }

        /* Initialize TTS HTTP API (POST /api/tts/speak) */
        err = tts_http_api_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TTS HTTP API initialized (POST /api/tts/speak)");
        } else {
            ESP_LOGW(TAG, "TTS HTTP API init failed: %s", esp_err_to_name(err));
        }

        /* Advertise only after the identity endpoint is registered. */
        err = device_info_api_register();
        bool identity_ready = err == ESP_OK;
        focuslamp_discovery_set_identity_ready(identity_ready);
        if (!identity_ready) {
            ESP_LOGE(TAG, "Device info API registration failed: %s", esp_err_to_name(err));
        } else if (discovery_initialized) {
            err = focuslamp_discovery_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "FocusLamp discovery started");
            } else {
                ESP_LOGE(TAG, "FocusLamp discovery start failed: %s", esp_err_to_name(err));
            }
        }
    }
#endif

    /* Step 2a: Initialize local FocusLamp services (no remote base ping). */
    err = focuslamp_bridge_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "FocusLamp bridge initialized");
    } else {
        ESP_LOGW(TAG, "FocusLamp bridge init failed: %s", esp_err_to_name(err));
    }

    /* Start async chat state reporter (queue + task) */
    s_chat_state_queue = xQueueCreate(CHAT_STATE_QUEUE_LEN, sizeof(chat_state_report_t));
    if (s_chat_state_queue) {
        if (xTaskCreate(chat_state_task, "chat_state", 3072, NULL,
                        tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create chat state task");
            vQueueDelete(s_chat_state_queue);
            s_chat_state_queue = NULL;
        } else {
            ESP_LOGI(TAG, "Chat state reporter started");
        }
    }

    /* Step 2b: Initialize Connection Manager (WiFi + WebSocket health monitoring) */
    conn_mgr_config_t conn_cfg = {
        .wifi_init_timeout_ms        = 30000,
        .wifi_reconnect_max_delay_ms = 60000,
        .ws_reconnect_max_delay_ms   = 60000,
        .monitor_interval_ms         = 5000,
    };
    err = connection_manager_init(&conn_cfg);
    if (err == ESP_OK) {
        err = connection_manager_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Connection manager started");
        } else {
            ESP_LOGW(TAG, "Connection manager start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Connection manager init failed: %s", esp_err_to_name(err));
    }

#if (WS_MANAGER_SERVER_ENABLE == 1)
    /* Step 2c: Initialize Heartbeat Service (WebSocket server client liveness) */
    err = heartbeat_service_start_server(30, 90);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Heartbeat service started");
    } else {
        ESP_LOGW(TAG, "Heartbeat service start failed: %s", esp_err_to_name(err));
    }

    /* Step 2d: Initialize Message Queue (reliable WebSocket message delivery) */
    err = message_queue_init(16, 3);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Message queue initialized");
    } else {
        ESP_LOGW(TAG, "Message queue init failed: %s", esp_err_to_name(err));
    }
#endif

    /* Step 3: Initialize Audio Bridge (I2S for TTS playback) */
    audio_bridge_config_t audio_cfg = AUDIO_BRIDGE_DEFAULT_CONFIG();
    err = audio_bridge_init(&audio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio Bridge init failed: %s", esp_err_to_name(err));
    }

    /* Step 4: Initialize Xiaozhi Manager (init only — do NOT start yet) */
    xiaozhi_manager_config_t xiaozhi_cfg = XIAOZHI_MANAGER_DEFAULT_CONFIG();
    xiaozhi_cfg.event_cb = xiaozhi_event_callback;
    xiaozhi_cfg.audio_cb = xiaozhi_audio_callback;

    err = xiaozhi_manager_init(&xiaozhi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi Manager init failed: %s", esp_err_to_name(err));
    }

    /* FocusLamp MCP tools are now registered inside xiaozhi_manager's
     * register_mcp_tools(), which is also re-run after each reconnect.
     * (Registered here before, they were lost after the MCP engine was
     * recreated on reconnect, so the cloud could no longer invoke
     * self.focuslamp.* tools.) */

    /* Step 5: Initialize Wake Word Engine BEFORE xiaozhi_manager_start()
     * so PCM callback is registered before mic_task starts running. */
    esp_err_t wake_dispatch_ret = wake_dispatch_init();
    if (wake_dispatch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Wake dispatcher init failed: %s; detections will only be logged",
                 esp_err_to_name(wake_dispatch_ret));
    }
    wake_word_engine_config_t wake_cfg = WAKE_WORD_ENGINE_DEFAULT_CONFIG();
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
    wake_cfg.default_lang = WAKE_WORD_LANG_EN;
#else
    wake_cfg.default_lang = WAKE_WORD_LANG_CN;
#endif
    wake_cfg.detect_cb = wake_word_detect_callback;
    wake_cfg.det_timeout_ms = 2000;
    /* 语音打断为后续测试项：barge_in_cb 暂不接线（见上方注释） */

    err = wake_word_engine_init(&wake_cfg);
    if (err == ESP_OK) {
        /* Register custom wake word commands based on compiled model */
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
        wake_word_engine_add_command(1, "focus");
#elif defined(CONFIG_SR_MN_CN_MULTINET7_QUANT)
        wake_word_engine_add_command(1, "ni hao xiao zhi");  /* 你好小智 */
        wake_word_engine_add_command(2, "xiao zhi xiao zhi"); /* 小智小智 */
#endif
        void *cmd_err = wake_word_engine_update_commands();
        if (cmd_err != NULL) {
            ESP_LOGW(TAG, "Some wake word commands could not be parsed");
            /* Error details are logged internally by wake_word_engine.
             * The returned pointer must be freed via wake_word_engine_free_error(). */
            wake_word_engine_free_error(cmd_err);
        }

        /* Start wake word detection */
        err = wake_word_engine_start();
        if (err == ESP_OK) {
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
            ESP_LOGI(TAG, "Wake word engine started — listening for 'focus'");
#elif defined(CONFIG_SR_MN_CN_MULTINET7_QUANT)
            ESP_LOGI(TAG, "Wake word engine started — listening for 'ni hao xiao zhi', 'xiao zhi xiao zhi'");
#endif
        } else {
            ESP_LOGE(TAG, "Wake word engine start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Wake Word Engine init failed: %s (model partition missing?)",
                 esp_err_to_name(err));
    }

    /* Voice has first claim on its large contiguous internal allocations.
     * Inputs are still initialized independently, even if wake setup failed.
     * Do not disable/remove radar/touch just to conceal an audio allocation bug. */
    peripheral_inputs_init();

    /* Step 6: Start Xiaozhi Manager — this starts mic_task which feeds PCM
     * to wake_word_engine via the callback registered in Step 5. */
    err = xiaozhi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi Manager start failed: %s", esp_err_to_name(err));
    }

    /* Add only a small shared-timer node after decoder/AFE initialization and
     * requesting voice startup; async mic setup may still be in progress.
     * The state engine itself does no I/O; demo_behaviors below applies the
     * companion effects in a separate task, without blocking its timer. */
    err = controller_modes_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Local modes unavailable: %s (other modules continue)", esp_err_to_name(err));
    }
#if (WS_MANAGER_SERVER_ENABLE == 1)
    if (ws_manager_server_is_running()) {
        esp_err_t mode_api_ret = mode_http_api_register();
        if (mode_api_ret != ESP_OK) {
            ESP_LOGE(TAG, "Mode API registration failed: %s", esp_err_to_name(mode_api_ret));
        }
    }
#endif

    /* LCD claims memory only after voice startup and mode initialization. */
    if (err == ESP_OK) {
        esp_err_t lcd_ret = lcd_driver_init();
        if (lcd_ret == ESP_OK) {
            lcd_ret = lcd_status_ui_start();
            if (lcd_ret != ESP_OK) {
                esp_err_t cleanup_ret = lcd_driver_deinit();
                if (cleanup_ret != ESP_OK) ESP_LOGW(TAG, "LCD cleanup deferred: %s", esp_err_to_name(cleanup_ret));
            }
        }
        if (lcd_ret != ESP_OK) ESP_LOGW(TAG, "Local LCD unavailable: %s (other modules continue)", esp_err_to_name(lcd_ret));
    }

    if (err == ESP_OK) {
        esp_err_t demo_ret = demo_behaviors_start();
        if (demo_ret != ESP_OK) ESP_LOGE(TAG, "Demo behavior worker unavailable: %s", esp_err_to_name(demo_ret));
    }

    ESP_LOGI(TAG, "=== Setup complete ===");

    /* Independent of mic callbacks: zero progress still produces diagnostics.
     * Counters are lifetime uint32 values; subtraction handles wraparound. */
    audio_bridge_mic_diagnostics_t previous_audio = {0};
    xiaozhi_mic_diagnostics_t previous_cloud = {0};
    audio_bridge_get_mic_diagnostics(&previous_audio);
    xiaozhi_manager_get_mic_diagnostics(&previous_cloud);
    unsigned health_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        audio_bridge_mic_diagnostics_t audio;
        xiaozhi_mic_diagnostics_t cloud;
        audio_bridge_get_mic_diagnostics(&audio);
        xiaozhi_manager_get_mic_diagnostics(&cloud);
#define AUDIO_DELTA(field) ((unsigned)(audio.field - previous_audio.field))
#define CLOUD_DELTA(field) ((unsigned)(cloud.field - previous_cloud.field))
        ESP_LOGI(TAG, "[MIC_DIAG] delta: read=%u rx=%u rx_err=%u partial=%u peak=%u "
                 "enc_try=%u enc=%u enc_err=%u enc_empty=%u",
                 AUDIO_DELTA(read_calls), AUDIO_DELTA(rx_frames), AUDIO_DELTA(read_errors),
                 AUDIO_DELTA(partial_reads), (unsigned)audio.peak,
                 AUDIO_DELTA(encode_calls), AUDIO_DELTA(encoded_frames),
                 AUDIO_DELTA(encode_errors), AUDIO_DELTA(empty_encoded));
        ESP_LOGI(TAG, "[UPLINK_DIAG] state=%d local=%d delta: cb=%u block_state=%u "
                 "block_tts=%u block_local=%u send_try=%u send_ok=%u send_fail=%u "
                 "resume_try=%u resume_ok=%u resume_fail=%u last_send_fail=%s last_resume_fail=%s "
                 "total_send_try=%u total_send_done=%u total_send_cancelled=%u inflight_ms=%" PRIi64
                 " cloud_event_age_ms=%" PRIi64 " audio_rx_age_ms=%" PRIi64
                 " queue_dwell_ms=%" PRIi64,
                 xiaozhi_manager_get_state(), audio_bridge_local_playing(),
                 CLOUD_DELTA(callbacks), CLOUD_DELTA(blocked_state),
                 CLOUD_DELTA(blocked_tts), CLOUD_DELTA(blocked_local),
                 CLOUD_DELTA(send_attempts), CLOUD_DELTA(send_ok), CLOUD_DELTA(send_failed),
                 CLOUD_DELTA(listen_attempts), CLOUD_DELTA(listen_ok), CLOUD_DELTA(listen_failed),
                 esp_err_to_name(cloud.last_send_error), esp_err_to_name(cloud.last_listen_error),
                 (unsigned)cloud.send_attempts, (unsigned)(cloud.send_ok + cloud.send_failed),
                 (unsigned)cloud.send_cancelled,
                 cloud.send_inflight_ms, cloud.cloud_event_age_ms,
                 cloud.audio_rx_age_ms, cloud.queue_dwell_ms);
        ESP_LOGI(TAG, "[UPLINK_QUEUE] queued=%u/8 high=%u enqueued=%u "
                 "drop_oldest=%u discarded=%u rejected=%u gen=%u users=%u "
                 "retiring=%d sender_stack_free=%u",
                 (unsigned)cloud.queued, (unsigned)cloud.queue_high_water,
                 (unsigned)cloud.enqueued, (unsigned)cloud.dropped_oldest,
                 (unsigned)cloud.discarded, (unsigned)cloud.rejected_frames,
                 (unsigned)cloud.generation, (unsigned)cloud.chat_users,
                 cloud.retiring, (unsigned)cloud.sender_stack_free);
#undef AUDIO_DELTA
#undef CLOUD_DELTA
        previous_audio = audio;
        previous_cloud = cloud;
        if (++health_tick % 2 != 0) continue; /* Preserve the existing 10 s health log. */

        xiaozhi_manager_state_t xiaozhi_state = xiaozhi_manager_get_state();
        ESP_LOGI(TAG, "Heap: %lu bytes | WiFi: %s | Xiaozhi: %d",
                 (unsigned long)esp_get_free_heap_size(),
                 wifi_manager_is_connected() ? "up" : "down",
                 xiaozhi_state);
    }
}
