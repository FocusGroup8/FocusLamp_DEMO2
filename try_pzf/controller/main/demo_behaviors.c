#include "demo_behaviors.h"
#include "controller_modes.h"
#include "led_service.h"
#include "radar_ld6002.h"
#include "focuslamp_discovery.h"
#include "xiaozhi_manager.h"
#include "local_reminder.h"
#include "audio_bridge.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define RADAR_FRESH_MS 3000
#define SEDENTARY_US (60LL * 1000000)
#define RETRY_US (5LL * 1000000)
static const char *TAG = "DEMO";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static demo_behaviors_status_t s_status;
static const char *WELCOME = "主人，跟Focus聊聊天吧～";

/* Local playback results carry the presence cycle. All access uses s_lock. */
static struct {
    uint32_t cycle;
    uint32_t revision;
    bool attempted;
    bool armed;
    bool submitted;
    bool local_output_done;
} s_reminder;

static void reset_reminder(void)
{
    portENTER_CRITICAL(&s_lock);
    bool unconfirmed = s_reminder.submitted && !s_reminder.local_output_done;
    uint32_t old_cycle = s_reminder.cycle;
    ++s_reminder.cycle;
    s_reminder.attempted = false;
    s_reminder.armed = false;
    s_reminder.submitted = false;
    s_reminder.local_output_done = false;
    portEXIT_CRITICAL(&s_lock);
    if (unconfirmed) {
        ESP_LOGW(TAG, "Sedentary cycle=%u ended: local output not confirmed",
                 (unsigned)old_cycle);
    }
}

void demo_behaviors_note_tts_text(const char *text)
{
    /* Kept for the existing app callback; cloud text cannot acknowledge a clip. */
    (void)text;
}

static bool local_allowed(uint32_t cycle)
{
    portENTER_CRITICAL(&s_lock);
    bool current = s_reminder.cycle == cycle && s_reminder.attempted;
    uint32_t revision = s_reminder.revision;
    portEXIT_CRITICAL(&s_lock);
    controller_modes_status_t mode;
    radar_ld6002_state_t radar = {0};
    int64_t now_ms = esp_timer_get_time() / 1000;
    return current && controller_modes_get_status(&mode) == ESP_OK &&
        mode.revision == revision && !mode.voice_active && mode.mode != APP_STATE_COMPANION &&
        xiaozhi_manager_get_state() != XIAOZHI_MANAGER_STATE_SPEAKING &&
        radar_ld6002_get_state(&radar) == ESP_OK && radar.has_presence &&
        radar.person_present && radar.presence_updated_ms > 0 &&
        now_ms >= radar.presence_updated_ms &&
        now_ms - radar.presence_updated_ms <= RADAR_FRESH_MS;
}

static void local_result(uint32_t cycle, esp_err_t ret)
{
    portENTER_CRITICAL(&s_lock);
    bool current = s_reminder.cycle == cycle;
    if (current) s_reminder.local_output_done = ret == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    if (current) ESP_LOGI(TAG, "Sedentary cycle=%u local_output_done=%d; result=%s; no retry",
                         (unsigned)cycle, ret == ESP_OK, esp_err_to_name(ret));
}

typedef struct { char text[384]; size_t len; bool overflow; } response_t;
static esp_err_t receive_response(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if ((size_t)event->data_len >= sizeof(r->text) - r->len) r->overflow = true;
        else {
            memcpy(r->text + r->len, event->data, event->data_len);
            r->len += event->data_len;
            r->text[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* One request at a time in this worker, never inside a timer or model lock.
 * The head endpoints can return HTTP 200 with code=1: check BOTH contracts. */
static esp_err_t head_post(const focuslamp_endpoint_t *peer, const char *path, const char *body)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u%s", peer->host, (unsigned)peer->port, path);
    response_t response = {0};
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 2000,
        .event_handler = receive_response, .user_data = &response,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body ? body : "{}", body ? strlen(body) : 2);
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK) return ret;
    if (status != 200 || response.overflow) return ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_ParseWithOpts(response.text, NULL, true);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = cJSON_IsObject(root) && cJSON_IsNumber(code) && code->valuedouble == 0;
    if (!ok) ESP_LOGW(TAG, "Head rejected %s: HTTP=%d response=%s", path, status, response.text);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool still_current(uint32_t revision)
{
    controller_modes_status_t now;
    return controller_modes_get_status(&now) == ESP_OK && now.revision == revision;
}

static bool can_announce(const controller_modes_status_t *mode)
{
    xiaozhi_manager_state_t state = xiaozhi_manager_get_state();
    return !mode->voice_active &&
        !audio_bridge_local_playing() &&
        (state == XIAOZHI_MANAGER_STATE_CONNECTED || state == XIAOZHI_MANAGER_STATE_LISTENING);
}

static void behavior_task(void *arg)
{
    (void)arg;
    uint32_t revision = UINT32_MAX;
    bool local_ready = false, head_ready = false, welcome_done = true;
    bool owned_breath = false;
    unsigned head_step = 0;
    char peer_host[16] = "";
    uint16_t peer_port = 0;
    int64_t local_retry = 0, head_retry = 0, speech_retry = 0;
    int64_t present_since = 0, last_sample = 0, last_summary = 0;
    for (;;) {
        controller_modes_status_t mode;
        if (controller_modes_get_status(&mode) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        int64_t now = esp_timer_get_time();
        if (mode.revision != revision) {
            revision = mode.revision;
            head_step = 0; head_ready = false;
            local_ready = false; local_retry = head_retry = speech_retry = 0;
            present_since = 0;
            reset_reminder();
            portENTER_CRITICAL(&s_lock);
            s_reminder.revision = revision;
            portEXIT_CRITICAL(&s_lock);
            /* Spoken mode requests get this line in their MCP result. Do not
             * inject over an active conversation. Idle HTTP starts enqueue it. */
            welcome_done = mode.mode != APP_STATE_COMPANION || mode.voice_active;
            ESP_LOGI(TAG, "Mode actions requested: mode=%s revision=%u; not completed yet",
                     controller_modes_name(mode.mode), (unsigned)revision);
        }

        if (!local_ready && now >= local_retry) {
            esp_err_t ret = ESP_OK;
            if (mode.mode == APP_STATE_COMPANION) {
                ret = led_service_turn_on(15); /* Existing raw scale: 0..30. */
                if (ret == ESP_OK) owned_breath = true;
                if (ret == ESP_OK) ret = led_service_set_effect(LED_EFFECT_BREATHING);
            } else if (owned_breath) {
                ret = led_service_turn_off();
                if (ret == ESP_OK) owned_breath = false;
            }
            if (ret == ESP_OK && still_current(revision)) {
                local_ready = true;
                ESP_LOGI(TAG, "Local mode actions applied: %s", controller_modes_name(mode.mode));
            } else {
                local_retry = now + RETRY_US;
                ESP_LOGW(TAG, "Local mode actions pending: %s", esp_err_to_name(ret));
            }
        }

        radar_ld6002_state_t radar = {0};
        esp_err_t radar_ret = radar_ld6002_get_state(&radar);
        now = esp_timer_get_time(); /* Sample can be newer than the loop's original time. */
        bool fresh = radar_ret == ESP_OK && radar.has_presence &&
            radar.presence_updated_ms > 0 && now/1000 >= radar.presence_updated_ms &&
            now/1000 - radar.presence_updated_ms <= RADAR_FRESH_MS;
        bool present = fresh && radar.person_present;
        if (mode.mode == APP_STATE_COMPANION || !present ||
            (last_sample && now - last_sample > 4000000)) {
            if (present_since) reset_reminder();
            present_since = 0;
        } else if (!present_since) present_since = now;
        last_sample = now;
        uint32_t elapsed = present_since ? (uint32_t)((now - present_since)/1000000) : 0;
        portENTER_CRITICAL(&s_lock);
        s_reminder.armed = present_since && !s_reminder.attempted &&
            now - present_since >= SEDENTARY_US;
        bool pending = s_reminder.armed;
        portEXIT_CRITICAL(&s_lock);
        if (!welcome_done && now >= speech_retry && can_announce(&mode)) {
            /* Companion welcome retains its existing retry policy. */
            esp_err_t ret = xiaozhi_manager_speak(WELCOME, 2);
            if (ret == ESP_OK) {
                welcome_done = true;
                ESP_LOGI(TAG, "Announcement queued (not playback confirmed): %s", WELCOME);
            } else ESP_LOGW(TAG, "Announcement pending: %s", esp_err_to_name(ret));
            speech_retry = now + RETRY_US;
        } else if (welcome_done && pending && still_current(revision) &&
                   !mode.voice_active &&
                   xiaozhi_manager_get_state() != XIAOZHI_MANAGER_STATE_SPEAKING) {
            /* One local attempt per presence cycle, even if enqueue fails. */
            portENTER_CRITICAL(&s_lock);
            s_reminder.attempted = true;
            s_reminder.armed = false;
            uint32_t cycle = s_reminder.cycle;
            portEXIT_CRITICAL(&s_lock);
            esp_err_t ret = local_reminder_submit(cycle, local_allowed, local_result);
            portENTER_CRITICAL(&s_lock);
            if (s_reminder.cycle == cycle) {
                s_reminder.submitted = ret == ESP_OK;
            }
            portEXIT_CRITICAL(&s_lock);
            pending = false;
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Sedentary cycle=%u local clip queued (not output confirmed)",
                         (unsigned)cycle);
            } else {
                ESP_LOGW(TAG, "Sedentary cycle=%u submission failed: %s; no retry this cycle",
                         (unsigned)cycle, esp_err_to_name(ret));
            }
        }

        focuslamp_endpoint_t peer;
        if (head_ready || now >= head_retry) {
        if (focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_HEAD, &peer) != ESP_OK) {
            head_ready = false; head_step = 0; peer_host[0] = '\0';
            head_retry = now + RETRY_US;
        } else {
            if (strcmp(peer_host, peer.host) || peer_port != peer.port) {
                snprintf(peer_host, sizeof(peer_host), "%s", peer.host);
                peer_port = peer.port; head_step = 0; head_ready = false; head_retry = 0;
            }
            if (!head_ready && now >= head_retry && still_current(revision)) {
                const char *path = "/api/mode/set";
                const char *body = mode.mode == APP_STATE_COMPANION ? "{\"mode\":\"companion\"}" :
                                   mode.mode == APP_STATE_FOCUS ? "{\"mode\":\"focus\"}" : "{\"mode\":\"normal\"}";
                if (head_step == 1) { path = "/api/led/off"; body = NULL; }
                if (head_step == 2) { path = "/api/display/on"; body = NULL; }
                if (head_step == 3) { path = "/api/eyes/expression"; body = "{\"expression\":\"happy\"}"; }
                if (head_step == 4) { path = "/api/camera/start"; body = NULL; }
                esp_err_t ret = head_post(&peer, path, body);
                focuslamp_endpoint_t current_peer;
                bool same_peer = focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_HEAD, &current_peer) == ESP_OK &&
                    !strcmp(peer.host, current_peer.host) && peer.port == current_peer.port;
                if (ret == ESP_OK && still_current(revision) && same_peer) {
                    ESP_LOGI(TAG, "Head action acknowledged: step=%u %s", head_step, path);
                    ++head_step;
                    head_ready = mode.mode != APP_STATE_COMPANION || head_step == 5;
                    if (head_ready) ESP_LOGI(TAG, "Head mode sync acknowledged: %s", controller_modes_name(mode.mode));
                } else {
                    head_retry = esp_timer_get_time() + RETRY_US;
                    ESP_LOGW(TAG, "Head action pending: step=%u %s -> %s", head_step, path, esp_err_to_name(ret));
                }
            }
        }
        }
        if (still_current(revision)) {
            portENTER_CRITICAL(&s_lock);
            s_status = (demo_behaviors_status_t) {
                .started = true, .revision = revision,
                .companion_local_ready = mode.mode == APP_STATE_COMPANION && local_ready,
                .head_sync_ready = head_ready, .radar_fresh = fresh, .radar_present = present,
                .sedentary_sec = elapsed, .sedentary_pending = s_reminder.armed,
                .sedentary_armed = s_reminder.armed,
                .sedentary_submitted = s_reminder.submitted,
                .sedentary_text_observed = false,
                .sedentary_local_output_done = s_reminder.local_output_done,
            };
            portEXIT_CRITICAL(&s_lock);
        }
        if (now - last_summary >= 10000000) {
            last_summary = now;
            demo_behaviors_status_t snapshot;
            demo_behaviors_get_status(&snapshot);
            ESP_LOGI(TAG, "Summary: mode=%s local=%d head=%d radar_fresh=%d present=%d sedentary=%us pending=%d armed=%d submitted=%d local_output_done=%d",
                     controller_modes_name(mode.mode), local_ready, head_ready, fresh, present, (unsigned)elapsed,
                     snapshot.sedentary_pending, snapshot.sedentary_armed,
                     snapshot.sedentary_submitted, snapshot.sedentary_local_output_done);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t demo_behaviors_start(void)
{
    portENTER_CRITICAL(&s_lock);
    bool started = s_status.started;
    portEXIT_CRITICAL(&s_lock);
    if (started) return ESP_OK;
    esp_err_t clip_ret = local_reminder_start();
    if (clip_ret != ESP_OK) return clip_ret;
    const uint32_t caps = MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL;
    ESP_LOGI(TAG, "Worker memory before: free=%u largest=%u", (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
    if (xTaskCreate(behavior_task, "demo_behaviors", 6144, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    s_status.started = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Demo worker started: stack=6144 priority=3; sedentary=60s Normal/Focus, no blocking timer work");
    ESP_LOGI(TAG, "Worker memory after: free=%u largest=%u", (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
    return ESP_OK;
}

void demo_behaviors_get_status(demo_behaviors_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    /* Expose playback-task results without waiting for the worker snapshot. */
    out->sedentary_pending = s_reminder.armed;
    out->sedentary_armed = s_reminder.armed;
    out->sedentary_submitted = s_reminder.submitted;
    out->sedentary_text_observed = false;
    out->sedentary_local_output_done = s_reminder.local_output_done;
    portEXIT_CRITICAL(&s_lock);
}
