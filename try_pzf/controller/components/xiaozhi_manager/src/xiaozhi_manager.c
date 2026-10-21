/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaozhi_display_interface.h"
#include "xiaozhi_manager.h"
#include "xiaozhi_manager_config.h"

#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "audio_bridge.h"
#include "device_controller.h"
#include "focuslamp_bridge.h"
#include "mipi_dsi_bridge.h"
#include "task_manager.h"
#include "wake_word_engine.h"
#include "uplink_buffer.h"
#include "speak_cycle.h"

static const char *TAG = "XIAOZHI_MGR";

static portMUX_TYPE s_mic_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_mic_diagnostics_t s_mic_diag;
static void uplink_get_diagnostics(xiaozhi_mic_diagnostics_t *out);

void xiaozhi_manager_get_mic_diagnostics(xiaozhi_mic_diagnostics_t *out) {
  if (!out) return;
  portENTER_CRITICAL(&s_mic_diag_lock);
  *out = s_mic_diag;
  portEXIT_CRITICAL(&s_mic_diag_lock);
  int64_t now_us = esp_timer_get_time();
  out->send_inflight_ms = out->send_started_us > 0 && now_us >= out->send_started_us
      ? (now_us - out->send_started_us) / 1000 : 0;
  uplink_get_diagnostics(out);
}

static _Atomic xiaozhi_manager_state_t s_state = XIAOZHI_MANAGER_STATE_IDLE;
static esp_xiaozhi_chat_handle_t s_chat_handle = 0;
/* Application-lifetime static storage: survives chat rebuilds and is never
 * freed while an asynchronous event or wake worker can still reference it. */
static StaticEventGroup_t s_audio_ready_storage;
static EventGroupHandle_t s_audio_ready_events;
#define AUDIO_CHANNEL_READY_BIT BIT0
#define AUDIO_CHANNEL_READY_WAIT_MS 3000
#define CHAT_EVENT_DRAINED_BIT BIT1
ESP_EVENT_DEFINE_BASE(XIAOZHI_MANAGER_PRIVATE_EVENTS);
static bool s_barrier_registered;
static uint32_t s_barrier_sequence, s_barrier_ack;
static esp_mcp_t *s_mcp_engine = NULL;
static bool s_owns_mcp = false;

static xiaozhi_manager_config_t s_config = {0};
static xiaozhi_display_cb_t s_display_cb = NULL;

/* Auto-reconnect state */
static esp_timer_handle_t s_reconnect_timer = NULL;
static int s_reconnect_count = 0;
static int s_current_reconnect_delay_ms = 0;
static bool s_reconnect_canceled = false;
/* True only while the one-shot reconnect timer is armed. A single transport
 * loss may surface as more than one DISCONNECTED event; keep those duplicate
 * events from consuming retry budget or notifying upper layers twice. */
static bool s_reconnect_pending = false;

/* Idle server closes are normally recovered by the websocket client's own
 * reconnect path (2 s delay). Keep the existing chat/MCP/mic objects alive
 * while that happens, but retain an upper-layer fallback long enough to cover
 * the 2 s delay plus one full ~10 s network/TLS attempt. */
#define TRANSPORT_RECONNECT_GRACE_MS 16000
/* A non-idle disconnect must stop/rebuild the old chat before the transport's
 * 2 s auto-reconnect can win the race. This is only the first attempt's timer;
 * the retry backoff state still starts at reconnect_delay_ms (normally 3 s). */
#define FULL_RECONNECT_FIRST_DELAY_MS 500
static bool s_waiting_transport_reconnect = false;
static bool s_transport_reconnect_forced = false;
static int64_t s_transport_reconnect_wait_started_us = 0;

/* MCP reconnect verification state */
static int s_mcp_reconnect_count = 0;

/* Saved chat config for reconnect — needed because chat_deinit destroys
 * the chat session and MCP engine, requiring full reinit on reconnect. */
static esp_xiaozhi_chat_config_t s_chat_config = {0};
static bool s_chat_config_saved = false;

/* Microphone state: tracks whether mic capture is active.
 * Mic is started when audio channel opens, stopped when it closes.
 * During TTS playback, mic is paused to avoid echo feedback.
 * Protected by s_mic_mutex for thread safety (accessed from multiple
 * event callbacks and the mic task context). */
static bool s_mic_active = false;
static _Atomic bool s_mic_paused_for_tts = false;
static SemaphoreHandle_t s_mic_mutex = NULL;

/* One application-lifetime pool and two workers, reused across chat rebuilds
 * and manager stop/start. No per-frame allocation or task deletion. The encoded
 * queue is CPU-only; prefer PSRAM, leaving internal RAM for task stacks/TLS. */
#define UPLINK_TASK_STACK_SIZE 8192
#define RECONNECT_TASK_STACK_SIZE 4096
#define CHAT_DRAIN_WAIT_MS 15000
static portMUX_TYPE s_uplink_lock = portMUX_INITIALIZER_UNLOCKED;
static uplink_buffer_t *s_uplink_buffer;
static TaskHandle_t s_uplink_task;
static TaskHandle_t s_reconnect_task;
static StaticSemaphore_t s_lifecycle_mutex_storage;
static SemaphoreHandle_t s_lifecycle_mutex;
static uint32_t s_uplink_generation;
static uint32_t s_reconnect_epoch;
static uint32_t s_reconnect_requested_epoch;
static unsigned s_chat_users;
static bool s_chat_retiring = true;
static _Atomic bool s_chat_rebuilding;
static bool s_uplink_accepting;
/* Local PCM owns speaker/mic admission without creating a cloud session. */
static bool s_local_audio_active;
static _Atomic(TaskHandle_t) s_chat_callback_task, s_chat_event_task;
static uint32_t s_uplink_enqueued, s_uplink_dropped, s_uplink_discarded;
static uint32_t s_uplink_rejected, s_uplink_high_water;
/* Application observations only: no socket-readiness or SDK lock telemetry.
 * Protected by the existing uplink lock and reset outside active transport
 * states. A timestamp of zero is valid; -1 means not observed this connection. */
static int64_t s_last_cloud_event_us = -1;
static int64_t s_last_audio_rx_us = -1;
static int64_t s_last_queue_dwell_ms = -1;

/* The existing speak worker owns a complete, isolated text-injection cycle.
 * This lock protects local progress, not SDK message identities. An old
 * transport is stopped and its default-loop events drained before reuse. */
static speak_cycle_t s_speak_cycle;
static bool s_normal_tts_active;
static bool s_normal_dialogue_active;
static bool s_transport_fresh;
static uint32_t s_transport_boundary_generation;
static bool s_speak_reset_pending, s_speak_reset_running;
static uint32_t s_speak_reset_generation;
#define SPEAK_FIRST_TTS_WAIT_US (30LL * 1000000)

static void speak_notify(void);
static void speak_cancel(speak_outcome_t outcome);
static void speak_reset_perform(uint32_t generation);
static void chat_rebuild_runtime(bool preserve_speak_queue);

static void uplink_task_func(void *arg);
static void reconnect_task_func(void *arg);

/* Only bookkeeping/copies in this critical section; never network, allocation,
 * logging or waits. Every user pins the handle before leaving this section. */
static void uplink_clear_locked(void) {
  ++s_uplink_generation;
  if (s_uplink_buffer) s_uplink_discarded += uplink_buffer_clear(s_uplink_buffer);
}

static void uplink_get_diagnostics(xiaozhi_mic_diagnostics_t *out) {
  portENTER_CRITICAL(&s_uplink_lock);
  out->enqueued = s_uplink_enqueued;
  out->dropped_oldest = s_uplink_dropped;
  out->discarded = s_uplink_discarded;
  out->rejected_frames = s_uplink_rejected;
  out->queued = s_uplink_buffer ? s_uplink_buffer->count : 0;
  out->queue_high_water = s_uplink_high_water;
  out->generation = s_uplink_generation;
  out->chat_users = s_chat_users;
  out->retiring = s_chat_retiring;
  int64_t now_us = esp_timer_get_time();
  out->cloud_event_age_ms = s_last_cloud_event_us >= 0 && now_us >= s_last_cloud_event_us
      ? (now_us - s_last_cloud_event_us) / 1000 : -1;
  out->audio_rx_age_ms = s_last_audio_rx_us >= 0 && now_us >= s_last_audio_rx_us
      ? (now_us - s_last_audio_rx_us) / 1000 : -1;
  out->queue_dwell_ms = s_last_queue_dwell_ms;
  portEXIT_CRITICAL(&s_uplink_lock);
  out->sender_stack_free = s_uplink_task ? uxTaskGetStackHighWaterMark(s_uplink_task) : 0;
}

static void uplink_suspend(bool retire_chat) {
  portENTER_CRITICAL(&s_uplink_lock);
  s_uplink_accepting = false;
  if (retire_chat) s_chat_retiring = true;
  uplink_clear_locked();
  portEXIT_CRITICAL(&s_uplink_lock);
}

static void manager_set_state_locked(xiaozhi_manager_state_t state) {
  s_state = state;
  if (state != XIAOZHI_MANAGER_STATE_CONNECTED &&
      state != XIAOZHI_MANAGER_STATE_LISTENING &&
      state != XIAOZHI_MANAGER_STATE_SPEAKING) {
    s_last_cloud_event_us = -1;
    s_last_audio_rx_us = -1;
    s_last_queue_dwell_ms = -1;
  }
  if (state != XIAOZHI_MANAGER_STATE_LISTENING) {
    s_uplink_accepting = false;
    uplink_clear_locked();
  }
}

static void manager_set_state(xiaozhi_manager_state_t state) {
  portENTER_CRITICAL(&s_uplink_lock);
  manager_set_state_locked(state);
  portEXIT_CRITICAL(&s_uplink_lock);
}

static bool manager_event_set_state(xiaozhi_manager_state_t state) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool allowed = !s_chat_rebuilding;
  if (allowed) manager_set_state_locked(state);
  portEXIT_CRITICAL(&s_uplink_lock);
  return allowed;
}

static void uplink_resume(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  if (!s_chat_retiring && s_state == XIAOZHI_MANAGER_STATE_LISTENING &&
      !s_mic_paused_for_tts && !s_local_audio_active &&
      !speak_cycle_busy(&s_speak_cycle)) {
    uplink_clear_locked();
    s_uplink_accepting = true;
  }
  portEXIT_CRITICAL(&s_uplink_lock);
}

esp_err_t xiaozhi_manager_local_audio_begin(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool allowed = !s_local_audio_active;
  if (allowed) {
    s_local_audio_active = true;
    s_uplink_accepting = false;
    uplink_clear_locked();
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void xiaozhi_manager_local_audio_end(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  s_local_audio_active = false;
  portEXIT_CRITICAL(&s_uplink_lock);
  /* Restore admission only if the existing cloud state permits listening.
   * Never send listen/start or overwrite the cloud/TTS state for local PCM. */
  uplink_resume();
  speak_notify();
}

static bool chat_pin(esp_xiaozhi_chat_handle_t *handle) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool allowed = s_chat_handle && !s_chat_retiring;
  if (allowed) {
    *handle = s_chat_handle;
    ++s_chat_users;
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  return allowed;
}

/* A normal wake/open/listen operation cannot enter between an injection's
 * idle check and reservation. Capture and pin are one critical section. */
static bool dialogue_pin(esp_xiaozhi_chat_handle_t *handle) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool allowed = s_chat_handle && !s_chat_retiring &&
      !s_local_audio_active && !speak_cycle_busy(&s_speak_cycle);
  if (allowed) {
    *handle = s_chat_handle;
    ++s_chat_users;
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  return allowed;
}

static void chat_unpin(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  --s_chat_users;
  portEXIT_CRITICAL(&s_uplink_lock);
}

/* Control calls also pin their handles: otherwise a wake/speak operation could
 * enter the old chat while the new audio worker is safely quiesced. No lock is
 * held across the SDK, including callbacks and audio-channel ready waits. */
static esp_err_t chat_listen(int mode) {
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_xiaozhi_chat_send_start_listening(handle, mode);
  chat_unpin();
  return ret;
}

static esp_err_t chat_stop_listening(void) {
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_xiaozhi_chat_send_stop_listening(handle);
  chat_unpin();
  return ret;
}

static esp_err_t chat_detect(const char *text) {
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_xiaozhi_chat_send_wake_word(handle, text);
  chat_unpin();
  return ret;
}

static esp_err_t chat_abort(void) {
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_xiaozhi_chat_send_abort_speaking(
      handle, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
  chat_unpin();
  return ret;
}

static esp_err_t chat_close_channel(void) {
  uplink_suspend(false);
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(handle);
  chat_unpin();
  return ret;
}

static bool chat_is_idle(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool idle = s_chat_users == 0;
  portEXIT_CRITICAL(&s_uplink_lock);
  return idle;
}

static bool in_chat_callback_task(void) {
  TaskHandle_t current = xTaskGetCurrentTaskHandle();
  return current == s_chat_callback_task || current == s_chat_event_task;
}

static esp_err_t chat_wait_idle(void) {
  int64_t start = esp_timer_get_time();
  while (!chat_is_idle()) {
    if (esp_timer_get_time() - start >= CHAT_DRAIN_WAIT_MS * 1000LL) {
      ESP_LOGW(TAG, "Chat drain timed out; retaining old chat and worker (no force delete)");
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return ESP_OK;
}

static esp_err_t uplink_runtime_init(void) {
  if (!s_lifecycle_mutex)
    s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
  if (!s_uplink_buffer) {
    s_uplink_buffer = heap_caps_calloc(1, sizeof(*s_uplink_buffer),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_uplink_buffer)
      s_uplink_buffer = heap_caps_calloc(1, sizeof(*s_uplink_buffer), MALLOC_CAP_8BIT);
    if (!s_uplink_buffer) return ESP_ERR_NO_MEM;
  }
  if (!s_uplink_task && xTaskCreate(uplink_task_func, "xiaozhi_uplink",
        UPLINK_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 4, &s_uplink_task) != pdPASS)
    return ESP_ERR_NO_MEM;
  if (!s_reconnect_task && xTaskCreate(reconnect_task_func, "xiaozhi_rebuild",
        RECONNECT_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 4, &s_reconnect_task) != pdPASS)
    return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG, "Async uplink ready: depth=%u frame_max=%u pool=%u bytes, "
           "internal_free=%u internal_largest=%u",
           UPLINK_QUEUE_DEPTH, UPLINK_FRAME_BYTES, (unsigned)sizeof(*s_uplink_buffer),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return ESP_OK;
}

/* Async TTS speak queue + task.
 * xiaozhi_manager_speak() enqueues a request and returns immediately, so
 * the calling task (REST /api/tts/speak handler, MCP notification.speak /
 * audio_speaker.play_tts) is never blocked. A dedicated task processes
 * requests one at a time running the state machine (abort current TTS /
 * open audio channel / inject text). This fixes the previous synchronous
 * behavior that blocked the HTTP handler for up to 6s, causing TTS
 * injection to fail silently and the device to appear unresponsive. */
#define SPEAK_TEXT_MAX_LEN 256
#define SPEAK_QUEUE_LEN 8
#define SPEAK_TASK_STACK_SIZE 4096

typedef struct {
  char text[SPEAK_TEXT_MAX_LEN];
  int priority;
} speak_request_t;

static QueueHandle_t s_speak_queue = NULL;
static TaskHandle_t s_speak_task = NULL;

static void speak_notify(void) {
  if (s_speak_task) xTaskNotifyGive(s_speak_task);
}

static void speak_cancel(speak_outcome_t outcome) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool changed = speak_cycle_finish(&s_speak_cycle,
      s_speak_cycle.generation, outcome);
  if (changed) {
    s_uplink_accepting = false;
    uplink_clear_locked();
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  if (changed) speak_notify();
}

/* SDK connection events have no source-instance payload. Stop/deinit first,
 * then this FIFO fence consumes all already-posted events before new init. */
static void chat_barrier_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data) {
  (void)arg;
  (void)base;
  (void)id;
  if (!data) return;
  portENTER_CRITICAL(&s_uplink_lock);
  s_barrier_ack = *(const uint32_t *)data;
  portEXIT_CRITICAL(&s_uplink_lock);
  xEventGroupSetBits(s_audio_ready_events, CHAT_EVENT_DRAINED_BIT);
}

static esp_err_t chat_drain_events(void) {
  uint32_t sequence;
  portENTER_CRITICAL(&s_uplink_lock);
  sequence = ++s_barrier_sequence;
  portEXIT_CRITICAL(&s_uplink_lock);
  xEventGroupClearBits(s_audio_ready_events, CHAT_EVENT_DRAINED_BIT);
  esp_err_t ret = esp_event_post(XIAOZHI_MANAGER_PRIVATE_EVENTS, 0,
      &sequence, sizeof(sequence), pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) return ret;
  int64_t started_us = esp_timer_get_time();
  do {
    xEventGroupWaitBits(s_audio_ready_events, CHAT_EVENT_DRAINED_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&s_uplink_lock);
    bool drained = s_barrier_ack == sequence;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (drained) return ESP_OK;
  } while (esp_timer_get_time() - started_us < 5000000);
  return ESP_ERR_TIMEOUT;
}

/* Forward declarations */
static void speak_task_func(void *arg);
static esp_err_t speak_process_request(const speak_request_t *req);
static esp_err_t send_wake_word_pinned(const char *wake_word);

/* Forward declarations for MCP tool callbacks */
static esp_mcp_value_t
mcp_tool_notification_speak(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_audio_speaker_set_volume(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_audio_speaker_play_tts(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_test_ping(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_test_echo(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * Auto-reconnect implementation
 *-------------------------------------------------------------*/
static void schedule_reconnect_if_needed(void);
static void cancel_reconnect(void);
static esp_err_t register_mcp_tools(void);
static void mic_force_stop(void);
static void reconnect_timer_callback(void *arg);
static void reconnect_perform(uint32_t epoch);
static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data);

static esp_err_t ensure_reconnect_timer(void) {
  if (s_reconnect_timer != NULL) {
    return ESP_OK;
  }

  const esp_timer_create_args_t timer_args = {
      .callback = reconnect_timer_callback, .name = "xiaozhi_reconnect"};
  return esp_timer_create(&timer_args, &s_reconnect_timer);
}

static bool start_transport_reconnect_grace(void) {
  if (!s_config.auto_reconnect) {
    return false;
  }

  esp_err_t ret = ensure_reconnect_timer();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create transport reconnect grace timer: %s",
             esp_err_to_name(ret));
    return false;
  }

  ret = esp_timer_start_once(
      s_reconnect_timer, (uint64_t)TRANSPORT_RECONNECT_GRACE_MS * 1000ULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start transport reconnect grace timer: %s",
             esp_err_to_name(ret));
    return false;
  }

  s_reconnect_canceled = false;
  s_reconnect_pending = true;
  s_waiting_transport_reconnect = true;
  s_transport_reconnect_forced = false;
  s_transport_reconnect_wait_started_us = esp_timer_get_time();
  ESP_LOGI(TAG,
           "Waiting for transport auto-reconnect (grace=%d ms, state=INITIALIZED)",
           TRANSPORT_RECONNECT_GRACE_MS);
  return true;
}

static esp_err_t request_full_reconnect_now(const char *reason) {
  if (!s_waiting_transport_reconnect || s_reconnect_timer == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t stop_err = esp_timer_stop(s_reconnect_timer);
  if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to stop transport reconnect grace timer: %s",
             esp_err_to_name(stop_err));
    return stop_err;
  }

  /* Keep s_waiting_transport_reconnect set: the timer callback uses it to
   * select the immediate full-rebuild fallback instead of a normal retry. */
  s_transport_reconnect_forced = true;
  esp_err_t start_err = esp_timer_start_once(s_reconnect_timer, 1);
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to request immediate full reconnect: %s",
             esp_err_to_name(start_err));
    s_reconnect_pending = false;
    s_waiting_transport_reconnect = false;
    s_transport_reconnect_forced = false;
    s_transport_reconnect_wait_started_us = 0;
    mic_force_stop();
    schedule_reconnect_if_needed();
    return start_err;
  }

  s_reconnect_pending = true;
  ESP_LOGW(TAG, "Immediate full reconnect requested: %s",
           reason ? reason : "unspecified");
  return ESP_OK;
}

static void reconnect_timer_callback(void *arg) {
  (void)arg;
  /* esp_timer and SDK event threads must never wait for an active upload. */
  portENTER_CRITICAL(&s_uplink_lock);
  TaskHandle_t worker = s_reconnect_canceled ? NULL : s_reconnect_task;
  s_reconnect_requested_epoch = s_reconnect_epoch;
  portEXIT_CRITICAL(&s_uplink_lock);
  if (worker) xTaskNotifyGive(worker);
}

static bool reconnect_request_valid(uint32_t epoch) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool valid = !s_reconnect_canceled && epoch == s_reconnect_epoch;
  portEXIT_CRITICAL(&s_uplink_lock);
  return valid;
}

static void reconnect_task_func(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_uplink_lock);
    uint32_t epoch = s_reconnect_requested_epoch;
    bool speak_reset = s_speak_reset_pending;
    uint32_t speak_generation = s_speak_reset_generation;
    if (speak_reset) {
      s_speak_reset_pending = false;
      s_speak_reset_running = true;
    }
    portEXIT_CRITICAL(&s_uplink_lock);
    if (speak_reset) {
      speak_reset_perform(speak_generation);
      portENTER_CRITICAL(&s_uplink_lock);
      s_speak_reset_running = false;
      portEXIT_CRITICAL(&s_uplink_lock);
      speak_notify();
    } else if (reconnect_request_valid(epoch)) reconnect_perform(epoch);
    /* CONNECTED can win while we drain an old call. In that case retain the
     * successfully reused transport instead of leaving its gate retired. */
    portENTER_CRITICAL(&s_uplink_lock);
    bool reused = !s_chat_rebuilding && s_chat_handle &&
        (s_state == XIAOZHI_MANAGER_STATE_CONNECTED ||
         s_state == XIAOZHI_MANAGER_STATE_LISTENING ||
         s_state == XIAOZHI_MANAGER_STATE_SPEAKING);
    if (reused) s_chat_retiring = false;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (reused) uplink_resume();
    xSemaphoreGive(s_lifecycle_mutex);
  }
}

static void reconnect_perform(uint32_t epoch) {
  if (s_state != XIAOZHI_MANAGER_STATE_INITIALIZED &&
      s_state != XIAOZHI_MANAGER_STATE_ERROR) return;
  uplink_suspend(true);
  while (chat_wait_idle() != ESP_OK) {
    /* No retry, delete or replacement while old calls own the old handle.
     * This dedicated worker may wait; SDK/event/timer tasks remain free. */
    if (!reconnect_request_valid(epoch)) return;
  }
  if (!reconnect_request_valid(epoch)) return;
  portENTER_CRITICAL(&s_uplink_lock);
  bool committed = epoch == s_reconnect_epoch && !s_reconnect_canceled &&
      (s_state == XIAOZHI_MANAGER_STATE_INITIALIZED ||
       s_state == XIAOZHI_MANAGER_STATE_ERROR);
  if (committed) {
    s_chat_rebuilding = true;
    manager_set_state_locked(XIAOZHI_MANAGER_STATE_CONNECTING);
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  if (!committed) return;
  /* The one-shot timer has fired, so a subsequent real failure may schedule
   * the next backoff step. */
  s_reconnect_pending = false;

  if (s_waiting_transport_reconnect) {
    bool forced_by_wake_word = s_transport_reconnect_forced;
    int64_t elapsed_ms = 0;
    if (s_transport_reconnect_wait_started_us > 0) {
      elapsed_ms =
          (esp_timer_get_time() - s_transport_reconnect_wait_started_us) / 1000;
    }
    s_waiting_transport_reconnect = false;
    s_transport_reconnect_forced = false;
    s_transport_reconnect_wait_started_us = 0;
    mic_force_stop();

    int next_reconnect_count = s_reconnect_count + 1;
    if (s_config.reconnect_max_retries > 0 &&
        next_reconnect_count > s_config.reconnect_max_retries) {
      ESP_LOGE(TAG, "Max reconnect attempts (%d) reached after transport "
                    "fallback timeout, giving up",
               s_config.reconnect_max_retries);
      manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
      s_chat_rebuilding = false;
      return;
    }
    s_reconnect_count = next_reconnect_count;
    s_current_reconnect_delay_ms = s_config.reconnect_delay_ms;

    if (forced_by_wake_word) {
      ESP_LOGW(TAG,
               "Transport reuse aborted by wake word after %lld ms; starting "
               "immediate full rebuild (attempt %d, state=%d)",
               (long long)elapsed_ms, s_reconnect_count, (int)s_state);
    } else {
      ESP_LOGW(TAG,
               "Transport auto-reconnect timed out after %lld ms; starting "
               "full rebuild fallback (attempt %d, state=%d)",
               (long long)elapsed_ms, s_reconnect_count, (int)s_state);
    }
  }

  ESP_LOGI(TAG, "Auto-reconnecting (attempt %d)...", s_reconnect_count);

  chat_rebuild_runtime(false);
}

static void chat_rebuild_runtime(bool preserve_speak_queue) {

  /* [FIX 2026-08-10] Drop queued speak requests on reconnect: TTS injects
   * enqueued before/while the connection dropped would otherwise fire the
   * moment the channel re-opens, sending stale broadcasts and racing the
   * re-connect handshake (observed "Websocket is not connected" during
   * auto-reconnect in long-run logs). speak_request_t is a value type, so
   * xQueueReset is memory-safe. */
  if (!preserve_speak_queue && s_speak_queue) {
    UBaseType_t pending = uxQueueMessagesWaiting(s_speak_queue);
    if (pending > 0) {
      ESP_LOGW(TAG, "Reconnect: dropping %u queued speak request(s)", (unsigned)pending);
      xQueueReset(s_speak_queue);
    }
  }

  /* Full deinit + reinit cycle to ensure MCP engine state is clean.
   * The previous approach of just calling chat_start() was insufficient
   * because: (1) the MCP engine retained stale session state from the
   * prior connection, causing ESP_ERR_INVALID_STATE on re-initialize;
   * (2) the mcp_chat_handle was cleared by chat_stop_runtime, making
   * connected_handler fail with "Invalid handle".
   *
   * By doing a full deinit/reinit, we create a fresh MCP engine and
   * transport, avoiding all stale state issues. */
  if (s_chat_handle != 0) {
    /* Unregister event handler before deinit to avoid receiving events
     * from the dying chat instance during the reinit process. */
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                 xiaozhi_esp_event_handler);
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
    s_mcp_engine = NULL;
    s_owns_mcp = false;
  }

  esp_err_t drained = chat_drain_events();
  if (drained != ESP_OK) {
    ESP_LOGE(TAG, "Old chat event drain failed: %s; new session remains gated",
             esp_err_to_name(drained));
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    schedule_reconnect_if_needed();
    return;
  }

  /* No old WS callback remains, and old default-loop events are now drained.
   * Clear local TTS state without sending listen/start or restarting capture. */
  portENTER_CRITICAL(&s_uplink_lock);
  s_normal_tts_active = false;
  s_normal_dialogue_active = false;
  s_transport_fresh = false;
  s_mic_paused_for_tts = false;
  ++s_transport_boundary_generation;
  portEXIT_CRITICAL(&s_uplink_lock);
  audio_bridge_flush_tts();
  wake_word_engine_resume();
  if (preserve_speak_queue && s_config.event_cb) {
    s_config.event_cb(XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED, NULL,
                      s_config.event_cb_ctx);
  }

  /* Re-register MCP tools (needed because chat_deinit destroyed the
   * MCP engine that owned all registered tools) */
  esp_err_t ret = esp_mcp_create(&s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to re-create MCP engine: %s", esp_err_to_name(ret));
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    schedule_reconnect_if_needed();
    return;
  }
  s_owns_mcp = true;

  ret = register_mcp_tools();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to re-register MCP tools: %s", esp_err_to_name(ret));
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    s_owns_mcp = false;
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    schedule_reconnect_if_needed();
    return;
  }

  /* Re-init chat with saved config (transfer MCP engine ownership) */
  if (!s_chat_config_saved) {
    ESP_LOGE(TAG, "No saved chat config for reconnect");
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    s_owns_mcp = false;
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    schedule_reconnect_if_needed();
    return;
  }

  esp_xiaozhi_chat_config_t new_config = s_chat_config;
  new_config.mcp_engine = s_mcp_engine;
  new_config.owns_mcp_engine = true;

  ret = esp_xiaozhi_chat_init(&new_config, &s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to re-init chat: %s", esp_err_to_name(ret));
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    s_owns_mcp = false;
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    schedule_reconnect_if_needed();
    return;
  }
  s_owns_mcp = false; /* chat owns MCP engine now */
  portENTER_CRITICAL(&s_uplink_lock);
  s_chat_retiring = false;
  s_chat_rebuilding = false;
  portEXIT_CRITICAL(&s_uplink_lock);

  /* Re-register event handler */
  ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                   xiaozhi_esp_event_handler, NULL);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to re-register event handler: %s",
             esp_err_to_name(ret));
  }

  /* Start new chat session */
  ret = esp_xiaozhi_chat_start(s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG,
             "Reconnect attempt %d failed at chat_start: %s (WS/TLS "
             "establishment failed or network unreachable, next retry)",
             s_reconnect_count, esp_err_to_name(ret));
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    /* Schedule next retry */
    schedule_reconnect_if_needed();
  } else {
    /* chat_start returned OK but the WS connect is asynchronous: stay in
     * CONNECTING until WEBSOCKET_EVENT_CONNECTED drives the state machine.
     * If the underlying esp_websocket keeps failing, the xiaozhi transport
     * emits CONNECT_FAILED and we reach schedule_reconnect via the event
     * handler. Log here for observability of the CONNECTING stall. */
    ESP_LOGI(TAG, "Reconnect attempt %d: chat_start accepted, waiting for "
                  "CONNECTED event",
             s_reconnect_count);
  }
}

/* Runs only in the existing rebuild worker, never a WS/event/timer callback.
 * Quiescing the real WS task plus the FIFO fence is the event-origin boundary;
 * a local cycle number alone cannot identify late, untagged SDK callbacks. */
static void speak_reset_perform(uint32_t generation) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool valid = s_speak_cycle.generation == generation &&
      speak_cycle_busy(&s_speak_cycle) && s_state != XIAOZHI_MANAGER_STATE_IDLE;
  portEXIT_CRITICAL(&s_uplink_lock);
  if (!valid) return;
  uplink_suspend(true);
  while (chat_wait_idle() != ESP_OK) {
    portENTER_CRITICAL(&s_uplink_lock);
    valid = s_speak_cycle.generation == generation &&
        speak_cycle_busy(&s_speak_cycle) && s_state != XIAOZHI_MANAGER_STATE_IDLE;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (!valid) return;
  }
  cancel_reconnect();
  portENTER_CRITICAL(&s_uplink_lock);
  s_chat_rebuilding = true;
  manager_set_state_locked(XIAOZHI_MANAGER_STATE_CONNECTING);
  portEXIT_CRITICAL(&s_uplink_lock);
  ESP_LOGI(TAG, "Speak cycle=%u: replacing old transport before releasing session gate",
           (unsigned)generation);
  chat_rebuild_runtime(true);
}

static void schedule_reconnect_if_needed(void) {
  if (!s_config.auto_reconnect) {
    return;
  }
  /* Allow reconnect even if s_chat_handle == 0 (after deinit cycle)
   * as long as we have saved config to reinit from. */
  if (s_chat_handle == 0 && !s_chat_config_saved) {
    return;
  }

  if (s_reconnect_pending) {
    ESP_LOGW(TAG, "Reconnect already pending; duplicate schedule ignored");
    return;
  }

  esp_err_t create_err = ensure_reconnect_timer();
  if (create_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create reconnect timer: %s",
             esp_err_to_name(create_err));
    return;
  }

  /* Calculate the next retry state without committing it. If timer start
   * fails, the retry budget and backoff must remain unchanged. */
  int next_reconnect_count = s_reconnect_count + 1;
  if (s_config.reconnect_max_retries > 0 &&
      next_reconnect_count > s_config.reconnect_max_retries) {
    ESP_LOGE(TAG, "Max reconnect attempts (%d) reached, giving up",
             s_config.reconnect_max_retries);
    return;
  }

  int next_reconnect_delay_ms;
  int scheduled_delay_ms;
  if (s_current_reconnect_delay_ms == 0) {
    next_reconnect_delay_ms = s_config.reconnect_delay_ms;
    scheduled_delay_ms = FULL_RECONNECT_FIRST_DELAY_MS;
  } else {
    next_reconnect_delay_ms = s_current_reconnect_delay_ms * 2;
    if (next_reconnect_delay_ms > s_config.reconnect_max_delay_ms) {
      next_reconnect_delay_ms = s_config.reconnect_max_delay_ms;
    }
    scheduled_delay_ms = next_reconnect_delay_ms;
  }

  esp_err_t start_err = esp_timer_start_once(
      s_reconnect_timer, (uint64_t)scheduled_delay_ms * 1000ULL);
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start reconnect timer: %s",
             esp_err_to_name(start_err));
    return;
  }

  s_reconnect_count = next_reconnect_count;
  s_current_reconnect_delay_ms = next_reconnect_delay_ms;
  s_reconnect_canceled = false;
  s_reconnect_pending = true;
  if (scheduled_delay_ms != s_current_reconnect_delay_ms) {
    ESP_LOGI(TAG,
             "Scheduling initial full reconnect in %d ms (attempt %d, "
             "retry_backoff_base=%d ms)",
             scheduled_delay_ms, s_reconnect_count,
             s_current_reconnect_delay_ms);
  } else {
    ESP_LOGI(TAG, "Scheduling reconnect in %d ms (attempt %d)",
             scheduled_delay_ms, s_reconnect_count);
  }
}

static void cancel_reconnect(void) {
  portENTER_CRITICAL(&s_uplink_lock);
  ++s_reconnect_epoch;
  s_reconnect_canceled = true;
  portEXIT_CRITICAL(&s_uplink_lock);
  s_reconnect_pending = false;
  s_waiting_transport_reconnect = false;
  s_transport_reconnect_forced = false;
  s_transport_reconnect_wait_started_us = 0;
  if (s_reconnect_timer != NULL) {
    esp_timer_stop(s_reconnect_timer);
  }
  s_reconnect_count = 0;
  s_current_reconnect_delay_ms = 0;
}

/*---------------------------------------------------------------
 * Display interface implementation
 *-------------------------------------------------------------*/
esp_err_t xiaozhi_display_register_callback(xiaozhi_display_cb_t cb) {
  s_display_cb = cb;
  return ESP_OK;
}

esp_err_t xiaozhi_display_notify(xiaozhi_display_event_t event, void *data) {
  if (s_display_cb) {
    return s_display_cb(event, data);
  }
  return ESP_OK;
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi chat audio callback
 *-------------------------------------------------------------*/
static void note_cloud_callback(bool audio) {
  portENTER_CRITICAL(&s_uplink_lock);
  if (!s_chat_rebuilding && !s_chat_retiring &&
      (s_state == XIAOZHI_MANAGER_STATE_CONNECTED ||
       s_state == XIAOZHI_MANAGER_STATE_LISTENING ||
       s_state == XIAOZHI_MANAGER_STATE_SPEAKING)) {
    if (audio) s_last_audio_rx_us = esp_timer_get_time();
    else s_last_cloud_event_us = esp_timer_get_time();
  }
  portEXIT_CRITICAL(&s_uplink_lock);
}

static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx) {
  portENTER_CRITICAL(&s_uplink_lock);
  bool current = !s_chat_rebuilding && !s_chat_retiring && !s_local_audio_active &&
      (s_speak_cycle.phase == SPEAK_PLAYING ||
       (s_speak_cycle.phase == SPEAK_IDLE && s_normal_tts_active));
  if (current) ++s_chat_users;
  portEXIT_CRITICAL(&s_uplink_lock);
  if (!current) return;
  if (data && len > 0) note_cloud_callback(true);
  if (s_config.audio_cb) {
    s_config.audio_cb(data, len, s_config.audio_cb_ctx);
  }
  chat_unpin();
}

/*---------------------------------------------------------------
 * Internal: Microphone callback — OPUS frame from audio_bridge
 *
 * Called by audio_bridge's mic_task for each encoded OPUS frame.
 * Forwards the frame to the xiaozhi server via WebSocket.
 *-------------------------------------------------------------*/
static esp_err_t uplink_enqueue(const uint8_t *data, size_t len) {
  bool local_playing = audio_bridge_local_playing();
  bool dropped = false;
  esp_err_t ret = ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&s_uplink_lock);
  if (!data || len == 0 || len > UPLINK_FRAME_BYTES) {
    ++s_uplink_rejected;
    ret = ESP_ERR_INVALID_ARG;
  } else if (local_playing || s_local_audio_active) {
    /* Do not retain pre-broadcast audio to upload after local speech ends. */
    uplink_clear_locked();
  } else if (s_uplink_accepting && !s_chat_retiring && s_uplink_task &&
             s_chat_handle && s_state == XIAOZHI_MANAGER_STATE_LISTENING &&
              !s_mic_paused_for_tts && !speak_cycle_busy(&s_speak_cycle) &&
              s_uplink_buffer) {
    if (uplink_buffer_push(s_uplink_buffer, data, len, s_uplink_generation,
                           esp_timer_get_time(), &dropped)) {
      ++s_uplink_enqueued;
      if (dropped) ++s_uplink_dropped;
      if (s_uplink_buffer->count > s_uplink_high_water)
        s_uplink_high_water = s_uplink_buffer->count;
      ret = ESP_OK;
    }
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  if (ret == ESP_OK) xTaskNotifyGive(s_uplink_task);
  return ret;
}

static void uplink_task_func(void *arg) {
  (void)arg;
  /* Private copy is never a pool slot: overflow cannot overwrite a frame
   * currently being masked/read by the synchronous WebSocket transport. */
  uplink_frame_t frame;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      esp_xiaozhi_chat_handle_t handle = 0;
      bool local_playing = audio_bridge_local_playing();
      portENTER_CRITICAL(&s_uplink_lock);
      bool ready = s_uplink_accepting && !s_chat_retiring && !local_playing &&
          !s_local_audio_active && !s_mic_paused_for_tts &&
          !speak_cycle_busy(&s_speak_cycle) &&
          s_state == XIAOZHI_MANAGER_STATE_LISTENING &&
          s_chat_handle;
      if (!ready) uplink_clear_locked();
      if (ready && uplink_buffer_pop(s_uplink_buffer, &frame)) {
        if (frame.generation == s_uplink_generation) {
          handle = s_chat_handle;
          ++s_chat_users;
          s_last_queue_dwell_ms = uplink_frame_dwell_ms(&frame, esp_timer_get_time());
        } else {
          ++s_uplink_discarded;
        }
      }
      portEXIT_CRITICAL(&s_uplink_lock);
      if (!handle) break;

      portENTER_CRITICAL(&s_mic_diag_lock);
      ++s_mic_diag.send_attempts;
      s_mic_diag.send_started_us = esp_timer_get_time();
      portEXIT_CRITICAL(&s_mic_diag_lock);
      esp_err_t ret = esp_xiaozhi_chat_send_audio_data(handle,
          (const char *)frame.data, frame.len);
      portENTER_CRITICAL(&s_mic_diag_lock);
      s_mic_diag.send_started_us = 0;
      if (ret == ESP_OK) ++s_mic_diag.send_ok;
      else {
        ++s_mic_diag.send_failed;
        s_mic_diag.last_send_error = ret;
      }
      portEXIT_CRITICAL(&s_mic_diag_lock);

      portENTER_CRITICAL(&s_uplink_lock);
      if (frame.generation == s_uplink_generation && ret != ESP_OK) {
        s_uplink_accepting = false;
        uplink_clear_locked();
      }
      portEXIT_CRITICAL(&s_uplink_lock);
      /* Always unpin, including a late result from an invalidated generation.
       * Generation changes neither cancel the SDK nor release its handle. */
      chat_unpin();
    }
  }
}

static void xiaozhi_mic_callback(const uint8_t *opus_data, int len, void *ctx) {
  (void)ctx;
  /* Only send OPUS when in LISTENING state (audio channel opened).
   * mic_task runs continuously to support wake word detection,
   * but OPUS is only forwarded when server is ready to receive. */
  bool listening = s_state == XIAOZHI_MANAGER_STATE_LISTENING;
  bool tts_paused = s_mic_paused_for_tts;
  bool local_playing = audio_bridge_local_playing();
  portENTER_CRITICAL(&s_uplink_lock);
  local_playing = local_playing || s_local_audio_active;
  portEXIT_CRITICAL(&s_uplink_lock);
  portENTER_CRITICAL(&s_mic_diag_lock);
  ++s_mic_diag.callbacks;
  if (!listening) ++s_mic_diag.blocked_state;
  if (tts_paused) ++s_mic_diag.blocked_tts;
  if (local_playing) ++s_mic_diag.blocked_local;
  portEXIT_CRITICAL(&s_mic_diag_lock);
  /* No SDK, blocking queue call, mutex wait or allocation on mic_task. */
  if (len > 0) (void)uplink_enqueue(opus_data, (size_t)len);
}

/*---------------------------------------------------------------
 * Internal: Start/stop microphone capture
 *-------------------------------------------------------------*/
static void mic_start_if_needed(void) {
  if (s_mic_mutex &&
      xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mic mutex in mic_start_if_needed");
    return;
  }
  if (s_mic_active || s_mic_paused_for_tts) {
    if (s_mic_mutex)
      xSemaphoreGive(s_mic_mutex);
    return;
  }
  esp_err_t ret = audio_bridge_mic_start(xiaozhi_mic_callback, NULL);
  if (ret == ESP_OK) {
    s_mic_active = true;
    ESP_LOGI(TAG, "Microphone capture started");
  } else {
    ESP_LOGE(TAG, "Failed to start microphone: %s", esp_err_to_name(ret));
  }
  if (s_mic_mutex)
    xSemaphoreGive(s_mic_mutex);
}

static void mic_pause_for_tts(void) {
  uplink_suspend(false);
  if (s_mic_mutex &&
      xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mic mutex in mic_pause_for_tts");
    return;
  }
  if (!s_mic_active || s_mic_paused_for_tts) {
    if (s_mic_mutex)
      xSemaphoreGive(s_mic_mutex);
    return;
  }
  /* Mark mic as paused for TTS — mic_task continues running so that
   * wake_word_engine's PCM callback (and AEC) keeps receiving audio.
   * Only OPUS→server transmission is blocked (checked in xiaozhi_mic_callback).
   */
  s_mic_paused_for_tts = true;
  ESP_LOGI(
      TAG,
      "Microphone paused for TTS playback (mic_task still running for AEC)");
  if (s_mic_mutex)
    xSemaphoreGive(s_mic_mutex);
}

static void mic_resume_after_tts(void) {
  if (s_mic_mutex &&
      xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mic mutex in mic_resume_after_tts");
    return;
  }
  if (!s_mic_paused_for_tts) {
    if (s_mic_mutex)
      xSemaphoreGive(s_mic_mutex);
    return;
  }
  uplink_suspend(false);
  s_mic_paused_for_tts = false;
  /* Mic task is still running — just resume sending OPUS to server */
  if (s_mic_mutex)
    xSemaphoreGive(s_mic_mutex);

  /* Notify server to start listening for user input.
   * Without this, the server does not process incoming audio after TTS ends,
   * causing the conversation to stall after the first reply. */
  if (s_chat_handle) {
    portENTER_CRITICAL(&s_mic_diag_lock);
    ++s_mic_diag.listen_attempts;
    portEXIT_CRITICAL(&s_mic_diag_lock);
    esp_err_t ret = chat_listen(ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    portENTER_CRITICAL(&s_mic_diag_lock);
    if (ret == ESP_OK) ++s_mic_diag.listen_ok;
    else {
      ++s_mic_diag.listen_failed;
      s_mic_diag.last_listen_error = ret;
    }
    portEXIT_CRITICAL(&s_mic_diag_lock);
    if (ret != ESP_OK) {
      /* [FIX 2026-08-10] Failed to resume listening (e.g. stale session_id):
       * reset the audio channel so the next conversation re-establishes a
       * fresh session instead of silently stalling after TTS. */
      ESP_LOGW(TAG,
               "send_start_listening after TTS failed: %s - closing channel to reset",
               esp_err_to_name(ret));
      chat_close_channel();
      return;
    }
  }

  uplink_resume();
  ESP_LOGI(TAG, "Microphone resumed after TTS playback");
}

static void mic_force_stop(void) {
  uplink_suspend(false);
  /* Force-stop mic regardless of state (used during disconnect/error cleanup).
   * Takes mutex if available, but proceeds even if mutex take fails. */
  bool mutex_taken = false;
  if (s_mic_mutex) {
    mutex_taken = (xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
  }
  if (s_mic_active || s_mic_paused_for_tts) {
    audio_bridge_mic_stop();
    /* The independent sender may still be inside the SDK. Only its actual
     * return clears send_started_us and releases its handle pin. */
    s_mic_active = false;
    s_mic_paused_for_tts = false;
    ESP_LOGI(TAG, "Microphone force-stopped");
  }
  if (mutex_taken && s_mic_mutex) {
    xSemaphoreGive(s_mic_mutex);
  }
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi chat event callback
 *-------------------------------------------------------------*/
static void xiaozhi_event_process(esp_xiaozhi_chat_event_t event,
                                  void *event_data, void *ctx);

static void xiaozhi_event_callback(esp_xiaozhi_chat_event_t event,
                                   void *event_data, void *ctx) {
  s_chat_callback_task = xTaskGetCurrentTaskHandle();
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return;
  xiaozhi_event_process(event, event_data, ctx);
  chat_unpin();
}

static void xiaozhi_event_process(esp_xiaozhi_chat_event_t event,
                                  void *event_data, void *ctx) {
  if (s_chat_rebuilding) return;
  /* Local transport errors are not evidence of cloud data reception. MCP,
   * hello and WS control frames bypass this business callback and are not
   * covered; TTS audio is observed separately in xiaozhi_audio_callback. */
  switch (event) {
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT:
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE:
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
    note_cloud_callback(false);
    break;
  default:
    break;
  }
  switch (event) {
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
    esp_xiaozhi_chat_tts_state_t *tts_state =
        (esp_xiaozhi_chat_tts_state_t *)event_data;
    if (tts_state) {
      switch (tts_state->state) {
      case ESP_XIAOZHI_CHAT_TTS_STATE_START: {
        portENTER_CRITICAL(&s_uplink_lock);
        uint32_t generation = s_speak_cycle.generation;
        bool injection = speak_cycle_busy(&s_speak_cycle);
        bool allowed = !s_chat_retiring && !s_chat_rebuilding;
        if (injection) {
          speak_cycle_expire(&s_speak_cycle, generation,
                             esp_timer_get_time(), SPEAK_FIRST_TTS_WAIT_US);
          allowed = allowed && speak_cycle_start(&s_speak_cycle, generation);
          if (!allowed) speak_cycle_finish(&s_speak_cycle, generation, SPEAK_OUTCOME_ERROR);
        } else {
          allowed = allowed && s_normal_dialogue_active &&
              (s_state == XIAOZHI_MANAGER_STATE_LISTENING ||
               s_state == XIAOZHI_MANAGER_STATE_SPEAKING);
          if (allowed) s_normal_tts_active = true;
        }
        portEXIT_CRITICAL(&s_uplink_lock);
        if (!allowed) {
          ESP_LOGW(TAG, "Ignoring TTS START outside an active dialogue/injection response");
          speak_notify();
          break;
        }
        if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_SPEAKING)) return;
        mic_pause_for_tts();
        /* Pause wake word engine during TTS to prevent loud echo audio
         * from crashing MultiNet's RNN-T beam search decoder.
         * Without AEC, TTS audio leaks into mic (peak ~9000 vs normal
         * speech ~200-700), causing FST beam search to dereference
         * invalid pointers and trigger Load access fault. */
        wake_word_engine_pause();
        /* 语音打断（barge-in）为后续测试项：已实现于 wake_word_engine 但
         * 暂不启用（未调用 set_tts_active），保持 TTS 期间无打断原行为。 */
        if (s_config.event_cb) {
          s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_START, NULL,
                            s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                               (void *)"speaking");
        if (injection) {
          ESP_LOGI(TAG, "Speak cycle=%u: TTS started", (unsigned)generation);
          speak_notify();
        }
        break;
      }
      case ESP_XIAOZHI_CHAT_TTS_STATE_STOP: {
        portENTER_CRITICAL(&s_uplink_lock);
        uint32_t generation = s_speak_cycle.generation;
        bool injection = speak_cycle_busy(&s_speak_cycle);
        bool normal = !injection && !s_chat_retiring && s_normal_tts_active &&
            s_normal_dialogue_active && s_state == XIAOZHI_MANAGER_STATE_SPEAKING;
        bool finished = false;
        if (injection) {
          speak_outcome_t outcome = s_speak_cycle.phase == SPEAK_PLAYING ?
              SPEAK_OUTCOME_DONE : SPEAK_OUTCOME_ERROR;
          finished = speak_cycle_finish(&s_speak_cycle, generation, outcome);
          s_uplink_accepting = false;
          uplink_clear_locked();
          /* Only local cleanup: never send listen/start before closing. */
          if (finished) s_mic_paused_for_tts = false;
        } else if (normal) s_normal_tts_active = false;
        portEXIT_CRITICAL(&s_uplink_lock);
        if (!finished && !normal) {
          ESP_LOGD(TAG, "Ignoring unmatched/late TTS STOP");
          break;
        }
        audio_bridge_flush_tts();
        wake_word_engine_resume();
        if (injection) {
          ESP_LOGI(TAG, "Speak cycle=%u: TTS ended; cleanup stays in speak/rebuild workers",
                   (unsigned)generation);
          speak_notify();
        } else {
          /* Normal (user) conversation: TTS finished — return to LISTENING
           * to continue the conversation. */
          if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_LISTENING)) return;
          mic_resume_after_tts();
        }
        /* Resume wake word engine after TTS ends.
         * wake_word_engine_resume() also cleans MultiNet state and
         * resets input buffer, discarding any TTS echo audio that
         * accumulated during the pause period. */
        if (s_config.event_cb) {
          s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_STOP, NULL,
                            s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                                (void *)(injection ? "idle" : "listening"));
        break;
      }
      case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
        portENTER_CRITICAL(&s_uplink_lock);
        bool active_tts = s_speak_cycle.phase == SPEAK_PLAYING ||
            (s_speak_cycle.phase == SPEAK_IDLE && s_normal_tts_active);
        portEXIT_CRITICAL(&s_uplink_lock);
        if (!active_tts) break;
        if (s_config.event_cb) {
          s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_SENTENCE,
                            (void *)tts_state->text, s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_TTS_TEXT,
                               (void *)tts_state->text);
        break;
      }
    }
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
    esp_xiaozhi_chat_text_data_t *text_data =
        (esp_xiaozhi_chat_text_data_t *)event_data;
    if (text_data && text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) {
      if (s_config.event_cb) {
        s_config.event_cb(XIAOZHI_MANAGER_EVENT_STT_TEXT,
                          (void *)text_data->text, s_config.event_cb_ctx);
      }
    }
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
    esp_xiaozhi_chat_error_info_t *err_info =
        (esp_xiaozhi_chat_error_info_t *)event_data;
    ESP_LOGE(TAG, "Chat error: code=%d source=%s", err_info->code,
             err_info->source ? err_info->source : "unknown");
    speak_cancel(SPEAK_OUTCOME_ERROR);
    /* Clean up mic on error — it may still be running */
    mic_force_stop();
    /* Transition to ERROR state, but allow recovery by reconnecting.
     * The error state is recoverable: call xiaozhi_manager_stop() then
     * xiaozhi_manager_start() to retry. */
    if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_ERROR)) return;
    schedule_reconnect_if_needed(); /* Auto-reconnect after error */
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_ERROR, err_info,
                        s_config.event_cb_ctx);
    }
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD: {
    const char *cmd = (const char *)event_data;
    ESP_LOGI(TAG, "System command: %s", cmd ? cmd : "null");
    /* Application decides whether to execute system commands */
    if (cmd && strcmp(cmd, "reboot") == 0) {
      ESP_LOGW(TAG, "Reboot command received from server - not executing in "
                    "current build");
    }
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI: {
    const char *emoji = (const char *)event_data;
    ESP_LOGD(TAG, "Emoji: %s", emoji ? emoji : "null");
    break;
  }
  default:
    break;
  }
}

/*---------------------------------------------------------------
 * Internal: esp_xiaozhi ESP event handler
 *-------------------------------------------------------------*/
static void xiaozhi_esp_event_process(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data);

static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
  s_chat_event_task = xTaskGetCurrentTaskHandle();
  esp_xiaozhi_chat_handle_t handle;
  if (!chat_pin(&handle)) return;
  xiaozhi_esp_event_process(arg, event_base, event_id, event_data);
  chat_unpin();
}

static void xiaozhi_esp_event_process(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
  if (s_chat_rebuilding) return;
  switch (event_id) {
  case ESP_XIAOZHI_CHAT_EVENT_CONNECTED: {
    bool transport_reused = s_waiting_transport_reconnect;
    int64_t transport_reconnect_ms = 0;
    if (transport_reused && s_transport_reconnect_wait_started_us > 0) {
      transport_reconnect_ms =
          (esp_timer_get_time() - s_transport_reconnect_wait_started_us) / 1000;
    }
    if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_CONNECTED)) return;
    portENTER_CRITICAL(&s_uplink_lock);
    s_transport_fresh = true;
    s_normal_dialogue_active = false;
    s_normal_tts_active = false;
    s_mic_paused_for_tts = false;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (s_audio_ready_events) xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    cancel_reconnect(); /* Reset reconnect counter on successful connection */
    if (transport_reused) {
      ESP_LOGI(TAG,
               "Transport auto-reconnect succeeded in %lld ms; chat/MCP/mic reused",
               (long long)transport_reconnect_ms);
    }
    if (s_mcp_reconnect_count > 0) {
      ESP_LOGI(TAG, "Reconnected to xiaozhi server (reconnect #%d)",
               s_mcp_reconnect_count);
    } else {
      ESP_LOGI(TAG, "Connected to xiaozhi server");
    }
    /* Start mic_task on (re)connection to support continuous wake word
     * detection. Idempotent: skips if already running (e.g., first connect
     * after xiaozhi_manager_start). */
    mic_start_if_needed();
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_CONNECTED, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                           (void *)"connected");
    speak_notify();
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED: {
    uplink_suspend(false);
    speak_cancel(SPEAK_OUTCOME_DISCONNECTED);
    portENTER_CRITICAL(&s_uplink_lock);
    s_normal_dialogue_active = false;
    s_normal_tts_active = false;
    s_transport_fresh = false;
    s_mic_paused_for_tts = false;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (s_audio_ready_events) xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    if (s_reconnect_pending) {
      ESP_LOGW(TAG, "Duplicate disconnect ignored (reconnect already pending)");
      break;
    }
    bool can_reuse_transport =
        (s_state == XIAOZHI_MANAGER_STATE_CONNECTED);
    /* INITIALIZED is deliberate: reconnect_timer_callback only permits
     * INITIALIZED/ERROR, so the grace timeout cannot be blocked by a stale
     * public CONNECTED state. The mic remains running on the reuse path. */
    if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_INITIALIZED)) return;
    s_mcp_reconnect_count++;
    ESP_LOGW(TAG, "Disconnected from xiaozhi server (total reconnects: %d)",
             s_mcp_reconnect_count);
    if (can_reuse_transport && start_transport_reconnect_grace()) {
      ESP_LOGI(TAG, "Idle disconnect: preserving chat/MCP/mic for transport reuse");
    } else {
      mic_force_stop();
      schedule_reconnect_if_needed(); /* Full rebuild with exponential backoff */
    }
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_DISCONNECTED, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                           (void *)"disconnected");
    break;
  }
  case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
    if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_LISTENING)) return;
    uplink_resume();
    if (s_audio_ready_events) xEventGroupSetBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    ESP_LOGI(TAG, "Audio channel opened");
    /* mic_task is already running (started in CONNECTED event).
     * OPUS forwarding enabled by s_state == LISTENING. */
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                           (void *)"listening");
    break;
  case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
    uplink_suspend(false);
    speak_cancel(SPEAK_OUTCOME_CANCELLED);
    portENTER_CRITICAL(&s_uplink_lock);
    s_normal_dialogue_active = false;
    s_normal_tts_active = false;
    s_mic_paused_for_tts = false;
    portEXIT_CRITICAL(&s_uplink_lock);
    audio_bridge_flush_tts();
    wake_word_engine_resume();
    speak_notify();
    /* chat_stop() can queue CLOSED while the old instance is being rebuilt.
     * It must not turn INITIALIZED/CONNECTING/ERROR into a false CONNECTED. */
    if (s_reconnect_pending ||
        (s_state != XIAOZHI_MANAGER_STATE_CONNECTED &&
         s_state != XIAOZHI_MANAGER_STATE_LISTENING &&
         s_state != XIAOZHI_MANAGER_STATE_SPEAKING)) {
      ESP_LOGW(TAG, "Ignoring audio-channel close during recovery (state=%d)",
               (int)s_state);
      break;
    }
    if (!manager_event_set_state(XIAOZHI_MANAGER_STATE_CONNECTED)) return;
    if (s_audio_ready_events) xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    ESP_LOGI(TAG, "Audio channel closed");
    /* Do NOT stop mic_task — keep running for wake word detection.
     * OPUS forwarding disabled by s_state != LISTENING. */
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"idle");
    break;
  case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
    uplink_suspend(false);
    ESP_LOGI(TAG, "Server goodbye received — conversation ended");
    /* Close audio channel cleanly. This will trigger AUDIO_CHANNEL_CLOSED
     * event which handles mic cleanup and state transition. */
    if (s_chat_handle) {
      chat_close_channel();
    }
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_SERVER_GOODBYE, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED, (void *)"idle");
    break;
  }
}

/*---------------------------------------------------------------
 * Internal: MCP tool - notification.speak
 * Proactive TTS injection, called by cloud server
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_notification_speak(const esp_mcp_property_list_t *properties) {
  const char *message =
      esp_mcp_property_list_get_property_string(properties, "message");
  int priority = esp_mcp_property_list_get_property_int(properties, "priority");

  ESP_LOGI(TAG, "[MCP] notification.speak: \"%s\" (priority=%d)",
           message ? message : "null", priority);

  /* Trigger proactive TTS injection via xiaozhi_manager_speak().
   * When the server calls this MCP tool, we treat the message as
   * a command text to be spoken, using the direct text injection
   * mechanism (listen detect + start listening). */
  if (message && strlen(message) > 0) {
    esp_err_t ret = xiaozhi_manager_speak(message, priority);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "[MCP] notification.speak failed: %s", esp_err_to_name(ret));
    }
  }

  return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - audio_speaker.set_volume
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_audio_speaker_set_volume(const esp_mcp_property_list_t *properties) {
  int volume = esp_mcp_property_list_get_property_int(properties, "volume");

  ESP_LOGI(TAG,
           "[MCP] audio_speaker.set_volume: %d (bidirectional verification: "
           "server→device volume command received)",
           volume);

  audio_bridge_set_volume(volume);
  return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - audio_speaker.play_tts
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_audio_speaker_play_tts(const esp_mcp_property_list_t *properties) {
  const char *text =
      esp_mcp_property_list_get_property_string(properties, "text");
  const char *preset_id =
      esp_mcp_property_list_get_property_string(properties, "preset_id");

  ESP_LOGI(TAG, "[MCP] audio_speaker.play_tts: text=\"%s\" preset_id=\"%s\"",
           text ? text : "null", preset_id ? preset_id : "null");

  /* Trigger proactive TTS injection for the provided text.
   * If text is provided, use it directly; otherwise, preset_id maps
   * to predefined reminder texts (e.g., "sedentary" -> "久坐提醒"). */
  const char *speak_text = text;
  if (!speak_text || strlen(speak_text) == 0) {
    /* Map preset_id to predefined text */
    if (preset_id && strcmp(preset_id, "sedentary") == 0) {
      speak_text = "提醒我休息";
    } else if (preset_id && strcmp(preset_id, "posture") == 0) {
      speak_text = "提醒我调整坐姿";
    } else if (preset_id && strcmp(preset_id, "focus") == 0) {
      speak_text = "提醒我集中注意力";
    } else if (preset_id && strcmp(preset_id, "hydration") == 0) {
      speak_text = "提醒我喝水";
    } else if (preset_id) {
      ESP_LOGW(TAG, "[MCP] play_tts: unknown preset_id \"%s\"", preset_id);
      return esp_mcp_value_create_bool(false);
    } else {
      ESP_LOGW(TAG, "[MCP] play_tts: no text or preset_id provided");
      return esp_mcp_value_create_bool(false);
    }
  }

  esp_err_t ret = xiaozhi_manager_speak(speak_text, 2);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "[MCP] play_tts failed: %s", esp_err_to_name(ret));
    return esp_mcp_value_create_bool(false);
  }

  return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - test.ping (silent, no TTS)
 * Simple connectivity check, returns current device state.
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_test_ping(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG,
           "[MCP] test.ping: silent connectivity check (reconnect_count=%d)",
           s_mcp_reconnect_count);
  /* Return structured info for verification */
  char result[64];
  snprintf(result, sizeof(result), "{\"status\":\"ok\",\"reconnects\":%d}",
           s_mcp_reconnect_count);
  return esp_mcp_value_create_string(result);
}

/*---------------------------------------------------------------
 * Internal: MCP tool - test.echo (silent, no TTS)
 * Echoes back the message parameter. Used for bidirectional
 * communication verification.
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_test_echo(const esp_mcp_property_list_t *properties) {
  const char *message =
      esp_mcp_property_list_get_property_string(properties, "message");

  ESP_LOGI(TAG, "[MCP] test.echo: \"%s\"", message ? message : "null");

  /* Echo the message back — this verifies server→device→server round-trip */
  if (message) {
    return esp_mcp_value_create_string(message);
  }
  return esp_mcp_value_create_bool(true);
}

/*---------------------------------------------------------------
 * Internal: Register all MCP tools
 *-------------------------------------------------------------*/
static esp_err_t register_mcp_tools(void) {
  esp_err_t ret = ESP_OK;

  /* notification.speak - proactive TTS injection */
  esp_mcp_tool_t *speak_tool = esp_mcp_tool_create(
      "self.notification.speak", "主动向用户播报信息，支持不同优先级",
      mcp_tool_notification_speak);
  if (!speak_tool) {
    ESP_LOGE(TAG, "Failed to create notification.speak tool");
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *msg_prop =
      esp_mcp_property_create("message", ESP_MCP_PROPERTY_TYPE_STRING);
  esp_mcp_tool_add_property(speak_tool, msg_prop);
  esp_mcp_property_t *prio_prop =
      esp_mcp_property_create_with_range("priority", 0, 3);
  esp_mcp_tool_add_property(speak_tool, prio_prop);
  esp_mcp_add_tool(s_mcp_engine, speak_tool);

  /* audio_speaker.set_volume */
  esp_mcp_tool_t *vol_tool = esp_mcp_tool_create(
      "self.audio_speaker.set_volume", "设置音频扬声器音量 (0-100)",
      mcp_tool_audio_speaker_set_volume);
  if (!vol_tool) {
    ESP_LOGE(TAG, "Failed to create set_volume tool");
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *vol_prop =
      esp_mcp_property_create_with_range("volume", 0, 100);
  esp_mcp_tool_add_property(vol_tool, vol_prop);
  esp_mcp_add_tool(s_mcp_engine, vol_tool);

  /* audio_speaker.play_tts */
  esp_mcp_tool_t *tts_tool =
      esp_mcp_tool_create("self.audio_speaker.play_tts", "播放预设文本语音",
                          mcp_tool_audio_speaker_play_tts);
  if (!tts_tool) {
    ESP_LOGE(TAG, "Failed to create play_tts tool");
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *text_prop =
      esp_mcp_property_create("text", ESP_MCP_PROPERTY_TYPE_STRING);
  esp_mcp_tool_add_property(tts_tool, text_prop);
  esp_mcp_property_t *preset_prop =
      esp_mcp_property_create("preset_id", ESP_MCP_PROPERTY_TYPE_STRING);
  esp_mcp_tool_add_property(tts_tool, preset_prop);
  esp_mcp_add_tool(s_mcp_engine, tts_tool);

  /* test.ping - silent connectivity check (no TTS) */
  esp_mcp_tool_t *ping_tool = esp_mcp_tool_create(
      "self.test.ping", "静默连通性检测，返回设备状态（不触发语音）",
      mcp_tool_test_ping);
  if (!ping_tool) {
    ESP_LOGE(TAG, "Failed to create test.ping tool");
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(s_mcp_engine, ping_tool);

  /* test.echo - bidirectional communication verification (no TTS) */
  esp_mcp_tool_t *echo_tool = esp_mcp_tool_create(
      "self.test.echo", "回显消息用于验证双向通信（不触发语音）",
      mcp_tool_test_echo);
  if (!echo_tool) {
    ESP_LOGE(TAG, "Failed to create test.echo tool");
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *echo_msg_prop =
      esp_mcp_property_create("message", ESP_MCP_PROPERTY_TYPE_STRING);
  esp_mcp_tool_add_property(echo_tool, echo_msg_prop);
  esp_mcp_add_tool(s_mcp_engine, echo_tool);

  ESP_LOGI(TAG, "Core MCP tools registered (notification.speak, "
                "audio_speaker.set_volume, audio_speaker.play_tts, "
                "test.ping, test.echo)");

  /* Register sub-module MCP tools */
  ret = task_manager_register_mcp_tools(s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register task_manager MCP tools");
    return ret;
  }

  ret = device_controller_register_mcp_tools(s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register device_controller MCP tools");
    return ret;
  }

  ret = mipi_dsi_bridge_register_mcp_tools(s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register mipi_dsi_bridge MCP tools");
    return ret;
  }

  /* FocusLamp (base board) MCP tools must be registered here too: the
   * reconnect path recreates the MCP engine and re-runs this function.
   * Previously they were only registered from app_main, so after any
   * reconnect the cloud's tools/list no longer contained self.focuslamp.*
   * and the LLM could not invoke focus.start / companion.start. */
  ret = focuslamp_bridge_register_mcp_tools(s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register focuslamp_bridge MCP tools");
    return ret;
  }

  ESP_LOGI(TAG, "All MCP tools registered successfully");
  return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
#if (XIAOZHI_MANAGER_ENABLE == 1)

esp_err_t xiaozhi_manager_init(const xiaozhi_manager_config_t *config) {
  ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
  ESP_RETURN_ON_FALSE(s_state == XIAOZHI_MANAGER_STATE_IDLE,
                      ESP_ERR_INVALID_STATE, TAG, "Already initialized");

  memcpy(&s_config, config, sizeof(s_config));
  if (!s_audio_ready_events) {
    s_audio_ready_events = xEventGroupCreateStatic(&s_audio_ready_storage);
  }
  ESP_RETURN_ON_FALSE(s_audio_ready_events, ESP_ERR_NO_MEM, TAG,
                      "Failed to create audio-ready event group");
  xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
  if (!s_barrier_registered) {
    esp_err_t barrier_ret = esp_event_handler_register(
        XIAOZHI_MANAGER_PRIVATE_EVENTS, 0, chat_barrier_handler, NULL);
    ESP_RETURN_ON_ERROR(barrier_ret, TAG, "Failed to register chat event drain");
    s_barrier_registered = true;
  }

  /* Create mic state protection mutex */
  s_mic_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_mic_mutex, ESP_ERR_NO_MEM, TAG,
                      "Failed to create mic mutex");

  esp_err_t ret;

  /* Step 0: Initialize sub-modules */
  ret = task_manager_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init task_manager: %s", esp_err_to_name(ret));
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  ret = device_controller_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init device_controller: %s", esp_err_to_name(ret));
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  ret = mipi_dsi_bridge_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init mipi_dsi_bridge: %s", esp_err_to_name(ret));
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  /* Step 1: Create MCP engine */
  ret = esp_mcp_create(&s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create MCP engine: %s", esp_err_to_name(ret));
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  s_owns_mcp = true;

  /* Step 2: Register core MCP tools */
  ret = register_mcp_tools();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register MCP tools");
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  /* Step 3: Get device info from server */
  esp_xiaozhi_chat_info_t info = {0};
  ret = esp_xiaozhi_chat_get_info(&info);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Device info retrieved: version=%s has_mqtt=%d has_ws=%d",
             info.current_version ? info.current_version : "N/A",
             info.has_mqtt_config, info.has_websocket_config);
  } else {
    ESP_LOGW(TAG, "Failed to get device info: %s (continuing anyway)",
             esp_err_to_name(ret));
  }

  /* Step 4: Configure and init chat */
  esp_xiaozhi_chat_config_t chat_config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
  chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
  chat_config.audio_callback = xiaozhi_audio_callback;
  chat_config.event_callback = xiaozhi_event_callback;
  chat_config.audio_callback_ctx = NULL;
  chat_config.event_callback_ctx = NULL;
  chat_config.mcp_engine = s_mcp_engine;
  chat_config.owns_mcp_engine = true;
  chat_config.has_mqtt_config =
      false; /* 强制使用WebSocket传输，避免UDP不通导致音频数据丢失 */
  chat_config.has_websocket_config = true;

  ret = esp_xiaozhi_chat_init(&chat_config, &s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init chat: %s", esp_err_to_name(ret));
    esp_xiaozhi_chat_free_info(&info);
    /* MCP engine is owned by chat now if init succeeded partially, skip destroy
     */
    s_mcp_engine = NULL;
    s_owns_mcp = false;
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }

  /* Save chat config for reconnect (must be after chat_init succeeds).
   * Note: mcp_engine and owns_mcp_engine are NOT saved here because
   * they change on each reconnect — reconnect_timer_callback sets them
   * dynamically based on the newly created MCP engine. */
  s_chat_config = chat_config;
  s_chat_config.mcp_engine = NULL;       /* Will be set during reconnect */
  s_chat_config.owns_mcp_engine = false; /* Will be set during reconnect */
  s_chat_config_saved = true;

  /* After chat_init with owns_mcp_engine=true, chat owns the MCP engine */
  s_owns_mcp = false;

  /* Step 5: Register ESP event handler for connection events */
  ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                   xiaozhi_esp_event_handler, NULL);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register ESP event handler: %s",
             esp_err_to_name(ret));
  }

  esp_xiaozhi_chat_free_info(&info);

  /* Create async TTS speak queue + task. Placed last so no error path
   * above needs to clean them up. */
  s_speak_queue = xQueueCreate(SPEAK_QUEUE_LEN, sizeof(speak_request_t));
  if (!s_speak_queue) {
    ESP_LOGE(TAG, "Failed to create speak queue");
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ESP_ERR_NO_MEM;
  }
  BaseType_t speak_task_ok = xTaskCreate(speak_task_func, "xiaozhi_speak",
                                         SPEAK_TASK_STACK_SIZE, NULL,
                                         tskIDLE_PRIORITY + 5, &s_speak_task);
  if (speak_task_ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create speak task");
    vQueueDelete(s_speak_queue);
    s_speak_queue = NULL;
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ESP_ERR_NO_MEM;
  }

  ret = uplink_runtime_init();
  if (ret != ESP_OK) {
    /* Workers/pool are fixed application-lifetime resources, retained for a
     * subsequent init retry. No audio can enter while s_chat_retiring is true. */
    ESP_LOGE(TAG, "Failed to initialize async uplink: %s", esp_err_to_name(ret));
    vTaskDelete(s_speak_task);
    s_speak_task = NULL;
    vQueueDelete(s_speak_queue);
    s_speak_queue = NULL;
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                 xiaozhi_esp_event_handler);
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
    s_chat_config_saved = false;
    s_mcp_engine = NULL;
    mipi_dsi_bridge_deinit();
    device_controller_deinit();
    task_manager_deinit();
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
    return ret;
  }
  portENTER_CRITICAL(&s_uplink_lock);
  s_chat_retiring = false;
  s_chat_rebuilding = false;
  portEXIT_CRITICAL(&s_uplink_lock);
  manager_set_state(XIAOZHI_MANAGER_STATE_INITIALIZED);
  ESP_LOGI(TAG, "Xiaozhi manager initialized successfully");

  return ESP_OK;
}

esp_err_t xiaozhi_manager_deinit(void) {
  if (s_audio_ready_events) xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
  if (s_state == XIAOZHI_MANAGER_STATE_IDLE) {
    return ESP_OK;
  }
  ESP_RETURN_ON_FALSE(!in_chat_callback_task(), ESP_ERR_INVALID_STATE, TAG,
                      "Deinit must run outside chat/event callbacks");
  ESP_RETURN_ON_FALSE(s_lifecycle_mutex &&
      xSemaphoreTake(s_lifecycle_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
      ESP_ERR_TIMEOUT, TAG, "Lifecycle worker is busy; resources retained");
  if (s_state == XIAOZHI_MANAGER_STATE_IDLE) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
  }
  uplink_suspend(true);
  speak_cancel(SPEAK_OUTCOME_CANCELLED);
  cancel_reconnect();
  if (chat_wait_idle() != ESP_OK) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_ERR_TIMEOUT;
  }
  s_chat_rebuilding = true;

  /* Stop microphone capture first */
  mic_force_stop();

  /* Cancel any pending reconnection */
  if (s_reconnect_timer != NULL) {
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
  }

  /* Stop chat if running */
  esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                               xiaozhi_esp_event_handler);
  if (s_chat_handle) {
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
  }
  esp_err_t drain_ret = chat_drain_events();
  if (drain_ret != ESP_OK) {
    xSemaphoreGive(s_lifecycle_mutex);
    return drain_ret;
  }
  portENTER_CRITICAL(&s_uplink_lock);
  speak_cycle_release(&s_speak_cycle, s_speak_cycle.generation);
  s_speak_reset_pending = false;
  s_normal_dialogue_active = false;
  s_normal_tts_active = false;
  s_transport_fresh = false;
  s_mic_paused_for_tts = false;
  portEXIT_CRITICAL(&s_uplink_lock);

  /* Deinitialize sub-modules */
  mipi_dsi_bridge_deinit();
  device_controller_deinit();
  task_manager_deinit();

  /* Clean up mic mutex */
  if (s_mic_mutex) {
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
  }

  /* Stop async speak task and free queue */
  if (s_speak_task) {
    vTaskDelete(s_speak_task);
    s_speak_task = NULL;
  }
  if (s_speak_queue) {
    vQueueDelete(s_speak_queue);
    s_speak_queue = NULL;
  }

  s_mcp_engine = NULL;
  s_owns_mcp = false;
  s_chat_config_saved = false;
  manager_set_state(XIAOZHI_MANAGER_STATE_IDLE);
  s_chat_rebuilding = false;
  s_display_cb = NULL;

  ESP_LOGI(TAG, "Xiaozhi manager deinitialized");
  xSemaphoreGive(s_lifecycle_mutex);
  return ESP_OK;
}

esp_err_t xiaozhi_manager_start(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_RETURN_ON_FALSE(s_lifecycle_mutex &&
      xSemaphoreTake(s_lifecycle_mutex, 0) == pdTRUE, ESP_ERR_INVALID_STATE,
      TAG, "Lifecycle worker is busy");
  if (!s_chat_handle || s_state == XIAOZHI_MANAGER_STATE_IDLE) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (!chat_is_idle()) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  portENTER_CRITICAL(&s_uplink_lock);
  s_chat_retiring = false;
  s_chat_rebuilding = false;
  portEXIT_CRITICAL(&s_uplink_lock);
  manager_set_state(XIAOZHI_MANAGER_STATE_CONNECTING);
  esp_err_t ret = esp_xiaozhi_chat_start(s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start chat: %s", esp_err_to_name(ret));
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    xSemaphoreGive(s_lifecycle_mutex);
    return ret;
  }

  ESP_LOGI(TAG, "Xiaozhi chat session started");
  xSemaphoreGive(s_lifecycle_mutex);
  return ESP_OK;
}

esp_err_t xiaozhi_manager_stop(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_RETURN_ON_FALSE(!in_chat_callback_task(), ESP_ERR_INVALID_STATE, TAG,
                      "Stop must run outside chat/event callbacks");
  ESP_RETURN_ON_FALSE(s_lifecycle_mutex &&
      xSemaphoreTake(s_lifecycle_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
      ESP_ERR_TIMEOUT, TAG, "Lifecycle worker is busy; resources retained");
  if (!s_chat_handle || s_state == XIAOZHI_MANAGER_STATE_IDLE) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  uplink_suspend(true);
  speak_cancel(SPEAK_OUTCOME_CANCELLED);
  /* Cancel any pending reconnection before stopping */
  cancel_reconnect();
  if (s_audio_ready_events) xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
  if (chat_wait_idle() != ESP_OK) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_ERR_TIMEOUT;
  }
  s_chat_rebuilding = true;

  esp_err_t ret = esp_xiaozhi_chat_stop(s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to stop chat: %s", esp_err_to_name(ret));
    s_chat_rebuilding = false;
    xSemaphoreGive(s_lifecycle_mutex);
    return ret;
  }

  ret = chat_drain_events();
  if (ret != ESP_OK) {
    xSemaphoreGive(s_lifecycle_mutex);
    return ret;
  }
  portENTER_CRITICAL(&s_uplink_lock);
  speak_cycle_release(&s_speak_cycle, s_speak_cycle.generation);
  s_speak_reset_pending = false;
  s_normal_dialogue_active = false;
  s_normal_tts_active = false;
  s_transport_fresh = false;
  s_mic_paused_for_tts = false;
  portEXIT_CRITICAL(&s_uplink_lock);
  audio_bridge_flush_tts();
  wake_word_engine_resume();

  manager_set_state(XIAOZHI_MANAGER_STATE_INITIALIZED);
  s_chat_rebuilding = false;
  ESP_LOGI(TAG, "Xiaozhi chat session stopped");
  xSemaphoreGive(s_lifecycle_mutex);
  return ESP_OK;
}

xiaozhi_manager_state_t xiaozhi_manager_get_state(void) { return s_state; }

esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word) {
  esp_xiaozhi_chat_handle_t handle;
  if (!dialogue_pin(&handle)) {
    ESP_LOGI(TAG, "Wake request deferred/rejected while broadcast or recovery owns the session");
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t ret = send_wake_word_pinned(wake_word);
  chat_unpin();
  return ret;
}

static esp_err_t send_wake_word_pinned(const char *wake_word) {
  if (audio_bridge_local_playing()) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");
  ESP_RETURN_ON_FALSE(wake_word, ESP_ERR_INVALID_ARG, TAG, "Invalid wake word");

  if (s_waiting_transport_reconnect) {
    ESP_LOGW(TAG,
             "Wake word detected while waiting for transport reconnect; "
             "forcing full rebuild now (wake word must be repeated after recovery)");
    esp_err_t reconnect_err =
        request_full_reconnect_now("wake word during transport reconnect");
    if (reconnect_err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to trigger full reconnect for wake word: %s",
               esp_err_to_name(reconnect_err));
      return reconnect_err;
    }
    return ESP_ERR_INVALID_STATE;
  }

  /* If currently speaking (TTS playing), abort speaking first before
   * sending the new wake word. This prevents overlapping audio streams
   * and ensures the server processes the new wake word correctly. */
  if (s_state == XIAOZHI_MANAGER_STATE_SPEAKING) {
    ESP_LOGI(TAG, "Aborting current TTS for new wake word: %s", wake_word);
    chat_abort();
  }

  /* [FIX 2026-08-10] Channel already open and server already listening:
   * sending another wake-detect would duplicate the listen state and could
   * confuse the server after an injected broadcast left the channel open.
   * Just make sure the server keeps listening and stay in the session. */
  if (s_state == XIAOZHI_MANAGER_STATE_LISTENING) {
    ESP_LOGI(TAG, "Wake word while already LISTENING - keep listening, no duplicate detect");
    esp_err_t ret = chat_listen(ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "start_listening while LISTENING failed: %s",
               esp_err_to_name(ret));
    } else {
      uplink_resume();
    }
    return ret;
  }

  /* If not in a state where we can send a wake word, open audio channel first.
   * This handles the case where audio channel was closed after goodbye. */
  if (s_state == XIAOZHI_MANAGER_STATE_CONNECTED) {
    ESP_RETURN_ON_FALSE(s_audio_ready_events, ESP_ERR_INVALID_STATE, TAG,
                        "Audio-ready event group not initialized");
    xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    esp_err_t ret = xiaozhi_manager_open_audio_channel();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open audio channel for wake word: %s",
               esp_err_to_name(ret));
      return ret;
    }
    /* open() posts OPENED asynchronously. Wait in the wake dispatcher, not
     * in the AFE callback; never assign LISTENING speculatively here. */
    EventBits_t ready = xEventGroupWaitBits(s_audio_ready_events,
        AUDIO_CHANNEL_READY_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(AUDIO_CHANNEL_READY_WAIT_MS));
    if (!(ready & AUDIO_CHANNEL_READY_BIT)) {
      ESP_LOGW(TAG, "Wake audio-channel ready event timed out after %d ms (state=%d)",
               AUDIO_CHANNEL_READY_WAIT_MS, (int)s_state);
      return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Wake audio-channel ready event confirmed");
  }

  /* Only send wake word when in LISTENING state (audio channel open) */
  if (s_state != XIAOZHI_MANAGER_STATE_LISTENING &&
      s_state != XIAOZHI_MANAGER_STATE_SPEAKING) {
    ESP_LOGW(TAG, "Cannot send wake word in state %d", s_state);
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Wake word detected: %s", wake_word);
  esp_err_t ret = chat_detect(wake_word);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to send wake word: %s", esp_err_to_name(ret));
    return ret;
  }

  /* After sending wake word detection, tell the server to start listening
   * for user voice input. Without this, the server only knows a wake word
   * was detected but does not begin processing incoming audio frames.
   * This aligns with the official esp_xiaozhi state machine:
   *   detect -> start listening (auto mode) */
  ret = chat_listen(ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send start_listening after wake word: %s",
             esp_err_to_name(ret));
    /* Non-fatal: server may still process audio based on detect message */
  }

  return ESP_OK;
}

esp_err_t xiaozhi_manager_interrupt_speaking(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  esp_xiaozhi_chat_handle_t handle;
  if (!dialogue_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = ESP_OK;
  if (s_state == XIAOZHI_MANAGER_STATE_SPEAKING) {
    ESP_LOGI(TAG, "Barge-in: interrupting current TTS");
    ret = chat_abort();
  }
  chat_unpin();
  return ret;
}

esp_err_t xiaozhi_manager_open_audio_channel(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Opening audio channel...");
  esp_xiaozhi_chat_handle_t opening_handle;
  bool injection_owner = xTaskGetCurrentTaskHandle() == s_speak_task;
  if (injection_owner ? !chat_pin(&opening_handle) : !dialogue_pin(&opening_handle))
    return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&s_uplink_lock);
  s_transport_fresh = false;
  if (!injection_owner) s_normal_dialogue_active = true;
  portEXIT_CRITICAL(&s_uplink_lock);
  uplink_suspend(false);
  esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(opening_handle, NULL, NULL, 0);
  if (ret != ESP_OK && !injection_owner) {
    portENTER_CRITICAL(&s_uplink_lock);
    if (s_chat_handle == opening_handle &&
        s_state == XIAOZHI_MANAGER_STATE_CONNECTED && !s_normal_tts_active)
      s_normal_dialogue_active = false;
    portEXIT_CRITICAL(&s_uplink_lock);
    speak_notify();
  }
  if (ret == ESP_ERR_TIMEOUT && s_chat_handle == opening_handle &&
      s_state == XIAOZHI_MANAGER_STATE_CONNECTED) {
    /* A connected WS socket is not a completed audio handshake. Do not leave
     * every subsequent wake retrying hello on this same unresponsive session.
     * Use the existing reconnect timer; never rebuild the chat in this call. */
    ESP_LOGW(TAG, "Audio hello timed out; scheduling full session recovery");
    manager_set_state(XIAOZHI_MANAGER_STATE_ERROR);
    if (s_audio_ready_events)
      xEventGroupClearBits(s_audio_ready_events, AUDIO_CHANNEL_READY_BIT);
    mic_force_stop();
    schedule_reconnect_if_needed();
  }
  chat_unpin();
  return ret;
}

esp_err_t xiaozhi_manager_close_audio_channel(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Closing audio channel...");
  esp_xiaozhi_chat_handle_t handle = 0;
  portENTER_CRITICAL(&s_uplink_lock);
  bool injection = speak_cycle_busy(&s_speak_cycle);
  if (injection) {
    speak_cycle_finish(&s_speak_cycle, s_speak_cycle.generation, SPEAK_OUTCOME_CANCELLED);
    s_uplink_accepting = false;
    uplink_clear_locked();
  } else if (s_chat_handle && !s_chat_retiring) {
    handle = s_chat_handle;
    ++s_chat_users;
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  if (injection) {
    speak_notify();
    return ESP_OK; /* Cancellation accepted; worker performs transport cleanup. */
  }
  if (!handle) return ESP_ERR_INVALID_STATE;
  uplink_suspend(false);
  esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(handle);
  chat_unpin();
  return ret;
}

esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");
  ESP_RETURN_ON_FALSE(data && data_len > 0, ESP_ERR_INVALID_ARG, TAG,
                      "Invalid audio data");

  return uplink_enqueue((const uint8_t *)data, data_len);
}

esp_mcp_t *xiaozhi_manager_get_mcp_engine(void) { return s_mcp_engine; }

/*---------------------------------------------------------------
 * Internal: Process a single speak request (runs in speak task context)
 *
 * One complete broadcast per worker iteration. Normal dialogues take priority;
 * already-accepted text remains in the existing FIFO instead of interrupting
 * user speech or being silently dropped by an injection-only guard.
 *-------------------------------------------------------------*/
static esp_err_t speak_transport_reset(uint32_t generation) {
  portENTER_CRITICAL(&s_uplink_lock);
  if (s_speak_cycle.generation != generation || !speak_cycle_busy(&s_speak_cycle)) {
    portEXIT_CRITICAL(&s_uplink_lock);
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t previous_boundary = s_transport_boundary_generation;
  s_speak_reset_generation = generation;
  s_speak_reset_pending = true;
  portEXIT_CRITICAL(&s_uplink_lock);
  xTaskNotifyGive(s_reconnect_task);
  for (;;) {
    portENTER_CRITICAL(&s_uplink_lock);
    bool current = s_speak_cycle.generation == generation &&
        speak_cycle_busy(&s_speak_cycle) && s_state != XIAOZHI_MANAGER_STATE_IDLE;
    bool drained = s_transport_boundary_generation != previous_boundary &&
        !s_speak_reset_pending && !s_speak_reset_running;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (!current) return ESP_ERR_INVALID_STATE;
    if (drained) return ESP_OK;
    /* A stuck SDK call still owns its handle: never free it on a timer. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
  }
}

static esp_err_t speak_send(uint32_t generation, const char *text) {
  esp_xiaozhi_chat_handle_t handle = 0;
  portENTER_CRITICAL(&s_uplink_lock);
  if (s_speak_cycle.generation == generation && !s_chat_retiring && s_chat_handle &&
      (s_speak_cycle.phase == SPEAK_WAITING || s_speak_cycle.phase == SPEAK_PLAYING)) {
    handle = s_chat_handle;
    ++s_chat_users;
  }
  portEXIT_CRITICAL(&s_uplink_lock);
  if (!handle) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = text ? esp_xiaozhi_chat_send_wake_word(handle, text) :
      esp_xiaozhi_chat_send_start_listening(handle, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
  chat_unpin();
  return ret;
}

static esp_err_t speak_process_request(const speak_request_t *req) {
  uint32_t generation = 0;
  bool fresh = false, logged_deferred = false;
  for (;;) {
    bool local_playing = audio_bridge_local_playing();
    portENTER_CRITICAL(&s_uplink_lock);
    bool stopped = s_state == XIAOZHI_MANAGER_STATE_IDLE ||
        (s_state == XIAOZHI_MANAGER_STATE_INITIALIZED && s_reconnect_canceled);
    bool ready = s_chat_handle && !s_chat_retiring && !s_chat_rebuilding &&
        s_state == XIAOZHI_MANAGER_STATE_CONNECTED && !s_normal_dialogue_active &&
        !s_normal_tts_active && s_chat_users == 0 && !local_playing &&
        !s_local_audio_active;
    bool reserved = ready && speak_cycle_begin(&s_speak_cycle, &generation);
    if (reserved) {
      fresh = s_transport_fresh;
      s_uplink_accepting = false;
      uplink_clear_locked();
    }
    portEXIT_CRITICAL(&s_uplink_lock);
    if (stopped) return ESP_ERR_INVALID_STATE;
    if (reserved) break;
    if (!logged_deferred) {
      ESP_LOGI(TAG, "Speak request remains queued/deferred until normal dialogue and recovery are idle");
      logged_deferred = true;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
  }
  ESP_LOGI(TAG, "Speak cycle=%u reserved: prio=%d", (unsigned)generation, req->priority);
  esp_err_t ret = ESP_OK;
  if (!fresh) ret = speak_transport_reset(generation);
  if (ret == ESP_OK) {
    /* New transport connection is asynchronous. No SDK handle is held while
     * this worker waits, so teardown and its callbacks can make progress. */
    for (;;) {
      portENTER_CRITICAL(&s_uplink_lock);
      bool current = s_speak_cycle.generation == generation &&
          s_speak_cycle.phase == SPEAK_OPENING;
      bool connected = s_state == XIAOZHI_MANAGER_STATE_CONNECTED &&
          !s_chat_retiring && !s_chat_rebuilding;
      bool failed = s_state == XIAOZHI_MANAGER_STATE_ERROR ||
          s_state == XIAOZHI_MANAGER_STATE_IDLE;
      portEXIT_CRITICAL(&s_uplink_lock);
      if (!current || failed) { ret = ESP_ERR_INVALID_STATE; break; }
      if (connected) break;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
  }
  bool attempted_open = false;
  if (ret == ESP_OK) {
    attempted_open = true;
    ret = xiaozhi_manager_open_audio_channel();
  }
  if (ret == ESP_OK) {
    int64_t opened_us = esp_timer_get_time();
    for (;;) {
      portENTER_CRITICAL(&s_uplink_lock);
      bool registered = s_state == XIAOZHI_MANAGER_STATE_LISTENING &&
          speak_cycle_wait(&s_speak_cycle, generation, esp_timer_get_time());
      bool current = s_speak_cycle.generation == generation &&
          s_speak_cycle.phase == SPEAK_OPENING;
      portEXIT_CRITICAL(&s_uplink_lock);
      if (registered) break;
      if (!current || esp_timer_get_time() - opened_us >= AUDIO_CHANNEL_READY_WAIT_MS * 1000LL) {
        ret = ESP_ERR_INVALID_STATE;
        break;
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
  }
  if (ret == ESP_OK) ret = speak_send(generation, req->text);
  if (ret == ESP_OK) ret = speak_send(generation, NULL);
  if (ret != ESP_OK) speak_cancel(SPEAK_OUTCOME_ERROR);
  else ESP_LOGI(TAG, "Speak cycle=%u submitted; waiting for cloud TTS (not playback confirmed)",
                (unsigned)generation);

  speak_outcome_t outcome;
  for (;;) {
    portENTER_CRITICAL(&s_uplink_lock);
    bool current = s_speak_cycle.generation == generation &&
        speak_cycle_busy(&s_speak_cycle);
    bool expired = speak_cycle_expire(&s_speak_cycle, generation,
        esp_timer_get_time(), SPEAK_FIRST_TTS_WAIT_US);
    bool closing = s_speak_cycle.phase == SPEAK_CLOSING;
    outcome = s_speak_cycle.outcome;
    portEXIT_CRITICAL(&s_uplink_lock);
    if (!current) return ESP_ERR_INVALID_STATE;
    if (expired) ESP_LOGW(TAG, "Speak cycle=%u: no TTS START within first-response budget",
                          (unsigned)generation);
    if (closing) break;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  }

  /* Includes partial submission and timeout: detach from the old WS before
   * admitting any new normal speech or a later FIFO item. No text re-send. */
  if (attempted_open) {
    esp_err_t cleanup = speak_transport_reset(generation);
    if (cleanup != ESP_OK) return cleanup;
  }
  portENTER_CRITICAL(&s_uplink_lock);
  speak_cycle_release(&s_speak_cycle, generation);
  portEXIT_CRITICAL(&s_uplink_lock);
  ESP_LOGI(TAG, "Speak cycle=%u released after cleanup: outcome=%d",
           (unsigned)generation, (int)outcome);
  if (outcome == SPEAK_OUTCOME_DONE) return ESP_OK;
  return outcome == SPEAK_OUTCOME_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

/*---------------------------------------------------------------
 * Internal: Async speak task
 *-------------------------------------------------------------*/
static void speak_task_func(void *arg) {
  (void)arg;
  speak_request_t req;
  while (s_speak_task) {
    if (xQueueReceive(s_speak_queue, &req, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    esp_err_t err = speak_process_request(&req);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Speak \"%s\" NOT broadcast: %s (state=%d)", req.text,
               esp_err_to_name(err), (int)s_state);
    }
  }
  vTaskDelete(NULL);
}

esp_err_t xiaozhi_manager_speak(const char *text, int priority) {
  ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");
  ESP_RETURN_ON_FALSE(s_speak_queue, ESP_ERR_INVALID_STATE, TAG,
                      "Speak queue not ready");

  /* Asynchronous enqueue — return immediately. The speak task processes
   * the request in the background, so the REST handler / MCP tool is never
   * blocked for the up-to-6s that the old synchronous flow took. */
  speak_request_t req = {0};
  req.priority = priority;
  snprintf(req.text, sizeof(req.text), "%s", text);

  if (xQueueSend(s_speak_queue, &req, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Speak queue full, dropping \"%s\"", text);
    return ESP_ERR_INVALID_STATE;
  }

  return ESP_OK;
}

esp_err_t xiaozhi_manager_send_text(const char *text) {
  ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");
  return xiaozhi_manager_speak(text, 2);
}

esp_err_t xiaozhi_manager_start_listening(int mode) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Starting listening (mode=%d)", mode);
  esp_xiaozhi_chat_handle_t handle;
  if (!dialogue_pin(&handle)) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = chat_listen(mode);
  if (ret == ESP_OK) uplink_resume();
  chat_unpin();
  return ret;
}

esp_err_t xiaozhi_manager_stop_listening(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Stopping listening");
  esp_xiaozhi_chat_handle_t handle;
  if (!dialogue_pin(&handle)) return ESP_ERR_INVALID_STATE;
  uplink_suspend(false);
  esp_err_t ret = chat_stop_listening();
  chat_unpin();
  return ret;
}

#else /* XIAOZHI_MANAGER_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t xiaozhi_manager_init(const xiaozhi_manager_config_t *config) {
  (void)config;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xiaozhi_manager_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xiaozhi_manager_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
xiaozhi_manager_state_t xiaozhi_manager_get_state(void) {
  return XIAOZHI_MANAGER_STATE_IDLE;
}
esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word) {
  (void)wake_word;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_open_audio_channel(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_close_audio_channel(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len) {
  (void)data;
  (void)data_len;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_mcp_t *xiaozhi_manager_get_mcp_engine(void) { return NULL; }
esp_err_t xiaozhi_manager_speak(const char *text, int priority) {
  (void)text;
  (void)priority;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_send_text(const char *text) {
  (void)text;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_start_listening(int mode) {
  (void)mode;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_stop_listening(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* XIAOZHI_MANAGER_ENABLE */
