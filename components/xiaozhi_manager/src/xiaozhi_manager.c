/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
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
#include "mipi_dsi_bridge.h"
#include "task_manager.h"
#include "wake_word_engine.h"

static const char *TAG = "XIAOZHI_MGR";

static xiaozhi_manager_state_t s_state = XIAOZHI_MANAGER_STATE_IDLE;
static esp_xiaozhi_chat_handle_t s_chat_handle = 0;
static esp_mcp_t *s_mcp_engine = NULL;
static bool s_owns_mcp = false;

static xiaozhi_manager_config_t s_config = {0};
static xiaozhi_display_cb_t s_display_cb = NULL;

/* Auto-reconnect state */
static esp_timer_handle_t s_reconnect_timer = NULL;
static int s_reconnect_count = 0;
static int s_current_reconnect_delay_ms = 0;
static bool s_reconnect_canceled = false;

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
static bool s_mic_paused_for_tts = false;
static SemaphoreHandle_t s_mic_mutex = NULL;

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
static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data);

static void reconnect_timer_callback(void *arg) {
  (void)arg;
  if (s_reconnect_canceled) {
    return;
  }
  if (s_state != XIAOZHI_MANAGER_STATE_INITIALIZED &&
      s_state != XIAOZHI_MANAGER_STATE_ERROR) {
    return; /* Already connected or in progress */
  }
  ESP_LOGI(TAG, "Auto-reconnecting (attempt %d)...", s_reconnect_count);
  s_state = XIAOZHI_MANAGER_STATE_CONNECTING;

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

  /* Re-register MCP tools (needed because chat_deinit destroyed the
   * MCP engine that owned all registered tools) */
  esp_err_t ret = esp_mcp_create(&s_mcp_engine);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to re-create MCP engine: %s", esp_err_to_name(ret));
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
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
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
    schedule_reconnect_if_needed();
    return;
  }

  /* Re-init chat with saved config (transfer MCP engine ownership) */
  if (!s_chat_config_saved) {
    ESP_LOGE(TAG, "No saved chat config for reconnect");
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    s_owns_mcp = false;
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
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
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
    schedule_reconnect_if_needed();
    return;
  }
  s_owns_mcp = false; /* chat owns MCP engine now */

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
    ESP_LOGW(TAG, "Reconnect attempt %d failed: %s", s_reconnect_count,
             esp_err_to_name(ret));
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
    /* Schedule next retry */
    schedule_reconnect_if_needed();
  }
  /* If successful, CONNECTED event will be received via callback,
   * which resets the reconnect counter. */
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

  if (s_reconnect_timer == NULL) {
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_callback, .name = "xiaozhi_reconnect"};
    esp_timer_create(&timer_args, &s_reconnect_timer);
  }

  s_reconnect_count++;
  if (s_config.reconnect_max_retries > 0 &&
      s_reconnect_count > s_config.reconnect_max_retries) {
    ESP_LOGE(TAG, "Max reconnect attempts (%d) reached, giving up",
             s_config.reconnect_max_retries);
    return;
  }

  /* Exponential backoff with cap */
  if (s_current_reconnect_delay_ms == 0) {
    s_current_reconnect_delay_ms = s_config.reconnect_delay_ms;
  } else {
    s_current_reconnect_delay_ms *= 2;
    if (s_current_reconnect_delay_ms > s_config.reconnect_max_delay_ms) {
      s_current_reconnect_delay_ms = s_config.reconnect_max_delay_ms;
    }
  }

  s_reconnect_canceled = false;
  ESP_LOGI(TAG, "Scheduling reconnect in %d ms (attempt %d)",
           s_current_reconnect_delay_ms, s_reconnect_count);
  esp_timer_start_once(s_reconnect_timer,
                       s_current_reconnect_delay_ms * 1000ULL);
}

static void cancel_reconnect(void) {
  s_reconnect_canceled = true;
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
static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx) {
  if (s_config.audio_cb) {
    s_config.audio_cb(data, len, s_config.audio_cb_ctx);
  }
}

/*---------------------------------------------------------------
 * Internal: Microphone callback — OPUS frame from audio_bridge
 *
 * Called by audio_bridge's mic_task for each encoded OPUS frame.
 * Forwards the frame to the xiaozhi server via WebSocket.
 *-------------------------------------------------------------*/
static void xiaozhi_mic_callback(const uint8_t *opus_data, int len, void *ctx) {
  /* Only send OPUS when in LISTENING state (audio channel opened).
   * mic_task runs continuously to support wake word detection,
   * but OPUS is only forwarded when server is ready to receive. */
  if (s_state == XIAOZHI_MANAGER_STATE_LISTENING && !s_mic_paused_for_tts) {
    esp_xiaozhi_chat_send_audio_data(s_chat_handle, (const char *)opus_data,
                                     (size_t)len);
  }
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

static void mic_stop_if_needed(void) {
  if (s_mic_mutex &&
      xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mic mutex in mic_stop_if_needed");
    return;
  }
  if (!s_mic_active) {
    if (s_mic_mutex)
      xSemaphoreGive(s_mic_mutex);
    return;
  }
  audio_bridge_mic_stop();
  s_mic_active = false;
  s_mic_paused_for_tts = false;
  ESP_LOGI(TAG, "Microphone capture stopped");
  if (s_mic_mutex)
    xSemaphoreGive(s_mic_mutex);
}

static void mic_pause_for_tts(void) {
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
  s_mic_paused_for_tts = false;
  /* Mic task is still running — just resume sending OPUS to server */
  if (s_mic_mutex)
    xSemaphoreGive(s_mic_mutex);

  /* Notify server to start listening for user input.
   * Without this, the server does not process incoming audio after TTS ends,
   * causing the conversation to stall after the first reply. */
  if (s_chat_handle) {
    esp_xiaozhi_chat_send_start_listening(s_chat_handle,
                                          ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
  }

  ESP_LOGI(TAG, "Microphone resumed after TTS playback");
}

static void mic_force_stop(void) {
  /* Force-stop mic regardless of state (used during disconnect/error cleanup).
   * Takes mutex if available, but proceeds even if mutex take fails. */
  bool mutex_taken = false;
  if (s_mic_mutex) {
    mutex_taken = (xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
  }
  if (s_mic_active || s_mic_paused_for_tts) {
    audio_bridge_mic_stop();
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
static void xiaozhi_event_callback(esp_xiaozhi_chat_event_t event,
                                   void *event_data, void *ctx) {
  switch (event) {
  case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
    esp_xiaozhi_chat_tts_state_t *tts_state =
        (esp_xiaozhi_chat_tts_state_t *)event_data;
    if (tts_state) {
      switch (tts_state->state) {
      case ESP_XIAOZHI_CHAT_TTS_STATE_START:
        s_state = XIAOZHI_MANAGER_STATE_SPEAKING;
        mic_pause_for_tts();
        /* Wake word engine stays active — AEC cancels TTS echo.
         * Only mic (opus→server) is paused to avoid sending echo. */
        if (s_config.event_cb) {
          s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_START, NULL,
                            s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                               (void *)"speaking");
        break;
      case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
        /* TTS finished — return to LISTENING to continue conversation.
         * Aligned with official esp_xiaozhi state machine:
         *   kDeviceStateSpeaking -> kDeviceStateListening
         * This allows user to continue speaking without saying wake
         * word again. Conversation ends only on SERVER_GOODBYE or
         * AUDIO_CHANNEL_CLOSED. */
        s_state = XIAOZHI_MANAGER_STATE_LISTENING;
        /* Flush any stale OPUS frames from the decode queue.
         * Without this, leftover frames continue writing to I2S
         * after TTS has ended, causing ESP_ERR_TIMEOUT errors. */
        audio_bridge_flush_tts();
        mic_resume_after_tts();
        /* Wake word engine was never paused — AEC handled echo cancellation.
         * NOTE: AEC is currently disabled for diagnostic purposes,
         * so wake word detection may trigger on TTS echo. Monitor
         * for false triggers and re-enable AEC after tuning. */
        if (s_config.event_cb) {
          s_config.event_cb(XIAOZHI_MANAGER_EVENT_TTS_STOP, NULL,
                            s_config.event_cb_ctx);
        }
        xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                               (void *)"listening");
        break;
      case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
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
    /* Clean up mic on error — it may still be running */
    mic_force_stop();
    /* Transition to ERROR state, but allow recovery by reconnecting.
     * The error state is recoverable: call xiaozhi_manager_stop() then
     * xiaozhi_manager_start() to retry. */
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
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
static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
  switch (event_id) {
  case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
    s_state = XIAOZHI_MANAGER_STATE_CONNECTED;
    cancel_reconnect(); /* Reset reconnect counter on successful connection */
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
    break;
  case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
    s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
    mic_force_stop();
    s_mcp_reconnect_count++;
    ESP_LOGW(TAG, "Disconnected from xiaozhi server (total reconnects: %d)",
             s_mcp_reconnect_count);
    schedule_reconnect_if_needed(); /* Auto-reconnect with exponential backoff
                                     */
    if (s_config.event_cb) {
      s_config.event_cb(XIAOZHI_MANAGER_EVENT_DISCONNECTED, NULL,
                        s_config.event_cb_ctx);
    }
    xiaozhi_display_notify(XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,
                           (void *)"disconnected");
    break;
  case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
    s_state = XIAOZHI_MANAGER_STATE_LISTENING;
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
    s_state = XIAOZHI_MANAGER_STATE_CONNECTED;
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
    ESP_LOGI(TAG, "Server goodbye received — conversation ended");
    /* Close audio channel cleanly. This will trigger AUDIO_CHANNEL_CLOSED
     * event which handles mic cleanup and state transition. */
    if (s_chat_handle) {
      esp_xiaozhi_chat_close_audio_channel(s_chat_handle);
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

  /* The chat module will handle TTS synthesis automatically
   * when the server calls this tool with text content */
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

  /* TODO: integrate with TTS playback when audio hardware available */
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

  s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
  ESP_LOGI(TAG, "Xiaozhi manager initialized successfully");

  return ESP_OK;
}

esp_err_t xiaozhi_manager_deinit(void) {
  if (s_state == XIAOZHI_MANAGER_STATE_IDLE) {
    return ESP_OK;
  }

  /* Stop microphone capture first */
  mic_force_stop();

  /* Cancel any pending reconnection */
  cancel_reconnect();
  if (s_reconnect_timer != NULL) {
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
  }

  /* Stop chat if running */
  if (s_chat_handle) {
    esp_xiaozhi_chat_stop(s_chat_handle);
    esp_xiaozhi_chat_deinit(s_chat_handle);
    s_chat_handle = 0;
  }

  /* Deinitialize sub-modules */
  mipi_dsi_bridge_deinit();
  device_controller_deinit();
  task_manager_deinit();

  /* Clean up mic mutex */
  if (s_mic_mutex) {
    vSemaphoreDelete(s_mic_mutex);
    s_mic_mutex = NULL;
  }

  s_mcp_engine = NULL;
  s_owns_mcp = false;
  s_chat_config_saved = false;
  s_state = XIAOZHI_MANAGER_STATE_IDLE;
  s_display_cb = NULL;

  ESP_LOGI(TAG, "Xiaozhi manager deinitialized");
  return ESP_OK;
}

esp_err_t xiaozhi_manager_start(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  s_state = XIAOZHI_MANAGER_STATE_CONNECTING;
  esp_err_t ret = esp_xiaozhi_chat_start(s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start chat: %s", esp_err_to_name(ret));
    s_state = XIAOZHI_MANAGER_STATE_ERROR;
    return ret;
  }

  ESP_LOGI(TAG, "Xiaozhi chat session started");
  return ESP_OK;
}

esp_err_t xiaozhi_manager_stop(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  /* Cancel any pending reconnection before stopping */
  cancel_reconnect();

  esp_err_t ret = esp_xiaozhi_chat_stop(s_chat_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to stop chat: %s", esp_err_to_name(ret));
    return ret;
  }

  s_state = XIAOZHI_MANAGER_STATE_INITIALIZED;
  ESP_LOGI(TAG, "Xiaozhi chat session stopped");
  return ESP_OK;
}

xiaozhi_manager_state_t xiaozhi_manager_get_state(void) { return s_state; }

esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");
  ESP_RETURN_ON_FALSE(wake_word, ESP_ERR_INVALID_ARG, TAG, "Invalid wake word");

  /* If currently speaking (TTS playing), abort speaking first before
   * sending the new wake word. This prevents overlapping audio streams
   * and ensures the server processes the new wake word correctly. */
  if (s_state == XIAOZHI_MANAGER_STATE_SPEAKING) {
    ESP_LOGI(TAG, "Aborting current TTS for new wake word: %s", wake_word);
    esp_xiaozhi_chat_send_abort_speaking(
        s_chat_handle,
        ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
  }

  /* If not in a state where we can send a wake word, open audio channel first.
   * This handles the case where audio channel was closed after goodbye. */
  if (s_state == XIAOZHI_MANAGER_STATE_CONNECTED) {
    esp_err_t ret = xiaozhi_manager_open_audio_channel();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open audio channel for wake word: %s",
               esp_err_to_name(ret));
      return ret;
    }
  }

  /* Only send wake word when in LISTENING state (audio channel open) */
  if (s_state != XIAOZHI_MANAGER_STATE_LISTENING &&
      s_state != XIAOZHI_MANAGER_STATE_SPEAKING) {
    ESP_LOGW(TAG, "Cannot send wake word in state %d", s_state);
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Wake word detected: %s", wake_word);
  esp_err_t ret = esp_xiaozhi_chat_send_wake_word(s_chat_handle, wake_word);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to send wake word: %s", esp_err_to_name(ret));
    return ret;
  }

  /* After sending wake word detection, tell the server to start listening
   * for user voice input. Without this, the server only knows a wake word
   * was detected but does not begin processing incoming audio frames.
   * This aligns with the official esp_xiaozhi state machine:
   *   detect -> start listening (auto mode) */
  ret = esp_xiaozhi_chat_send_start_listening(
      s_chat_handle, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send start_listening after wake word: %s",
             esp_err_to_name(ret));
    /* Non-fatal: server may still process audio based on detect message */
  }

  return ESP_OK;
}

esp_err_t xiaozhi_manager_open_audio_channel(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Opening audio channel...");
  return esp_xiaozhi_chat_open_audio_channel(s_chat_handle, NULL, NULL, 0);
}

esp_err_t xiaozhi_manager_close_audio_channel(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Closing audio channel...");
  return esp_xiaozhi_chat_close_audio_channel(s_chat_handle);
}

esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");
  ESP_RETURN_ON_FALSE(data && data_len > 0, ESP_ERR_INVALID_ARG, TAG,
                      "Invalid audio data");

  return esp_xiaozhi_chat_send_audio_data(s_chat_handle, data, data_len);
}

esp_mcp_t *xiaozhi_manager_get_mcp_engine(void) { return s_mcp_engine; }

esp_err_t xiaozhi_manager_speak(const char *text, int priority) {
  ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");

  ESP_LOGI(TAG, "Speak request: \"%s\" (priority=%d)", text, priority);

  /* Use the notification.speak MCP tool mechanism.
   * In the xiaozhi protocol, proactive TTS injection is done
   * through the server-side tool call mechanism.
   * NOTE: esp_mcp_notify_log_message() is not supported by xiaozhi
   * transport (no emit_message in vtable). Log locally only. */
  if (s_mcp_engine) {
    ESP_LOGI(TAG, "Speak request logged locally (MCP emit unsupported): \"%s\"",
             text);
  }

  return ESP_OK;
}

esp_err_t xiaozhi_manager_start_listening(int mode) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Starting listening (mode=%d)", mode);
  return esp_xiaozhi_chat_send_start_listening(s_chat_handle, mode);
}

esp_err_t xiaozhi_manager_stop_listening(void) {
  ESP_RETURN_ON_FALSE(s_chat_handle, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  ESP_LOGI(TAG, "Stopping listening");
  return esp_xiaozhi_chat_send_stop_listening(s_chat_handle);
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
esp_err_t xiaozhi_manager_start_listening(int mode) {
  (void)mode;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_manager_stop_listening(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* XIAOZHI_MANAGER_ENABLE */
