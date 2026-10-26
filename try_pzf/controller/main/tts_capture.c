#include "tts_capture.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "websocket_manager.h"

#define CAPTURE_CAPACITY (512U * 1024U)
#define CAPTURE_MAX_US (60LL * 1000000)
#define CAPTURE_TEXT_BYTES 2048

typedef enum { CAP_IDLE, CAP_ARMED, CAP_RECORDING, CAP_SEALING, CAP_COMPLETE, CAP_ERROR } phase_t;
typedef struct {
    phase_t phase;
    uint32_t id, frames;
    size_t bytes, text_bytes;
    int64_t started_us;
    const char *error;
    bool text_truncated;
} capture_t;

typedef struct {
    capture_t capture;
    char text[CAPTURE_TEXT_BYTES];
} snapshot_t;

/* Only small metadata is protected here. No allocation, PCM/Opus/text copy,
 * HTTP operation or task wait is allowed inside this critical section. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static capture_t s_capture;
static char s_text[CAPTURE_TEXT_BYTES];
static uint8_t *s_packets;
static bool s_downloading;
static bool s_cloud_playing;
static unsigned s_writers;
static const char *TAG = "TTS_CAPTURE";

/* Helpers below require s_state_lock. No new worker/task is introduced. */
static void fail_locked(const char *reason)
{
    s_capture.phase = CAP_ERROR;
    s_capture.error = reason;
}

static void check_locked(int64_t now_us)
{
    if ((s_capture.phase == CAP_RECORDING || s_capture.phase == CAP_SEALING) &&
        now_us - s_capture.started_us > CAPTURE_MAX_US)
        fail_locked("reply_exceeds_60_seconds");
}

static void seal_locked(void)
{
    if (s_capture.phase == CAP_SEALING && !s_writers) {
        if (!s_capture.frames) fail_locked("tts_stopped_without_audio");
        else s_capture.phase = CAP_COMPLETE;
    }
}

static void writer_done(void)
{
    portENTER_CRITICAL(&s_state_lock);
    --s_writers;
    seal_locked();
    portEXIT_CRITICAL(&s_state_lock);
}

void tts_capture_start(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_cloud_playing = true;
    if (s_capture.phase == CAP_ARMED) {
        s_capture.phase = CAP_RECORDING;
        s_capture.started_us = now_us;
    } else if (s_capture.phase == CAP_RECORDING || s_capture.phase == CAP_SEALING)
        fail_locked("duplicate_tts_start");
    portEXIT_CRITICAL(&s_state_lock);
}

void tts_capture_packet(const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    int64_t now_us = esp_timer_get_time();
    uint8_t *out = NULL;
    portENTER_CRITICAL(&s_state_lock);
    check_locked(now_us);
    if (s_capture.phase == CAP_RECORDING) {
        if (len > UINT16_MAX || len + 2 > CAPTURE_CAPACITY - s_capture.bytes) {
            fail_locked("capture_buffer_full");
        } else {
            out = s_packets + s_capture.bytes;
            s_capture.bytes += len + 2;
            ++s_capture.frames;
            ++s_writers;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!out) return;
    /* Reserved bytes are private to this writer. STOP cannot publish and
     * HTTP cannot re-arm/free/download this buffer until writer_done(). */
    out[0] = (uint8_t)len;
    out[1] = (uint8_t)(len >> 8);
    memcpy(out + 2, data, len);
    writer_done();
}

void tts_capture_sentence(const char *text)
{
    if (!text) return;
    size_t n = strnlen(text, CAPTURE_TEXT_BYTES);
    char *out = NULL;
    portENTER_CRITICAL(&s_state_lock);
    if (s_capture.phase == CAP_RECORDING) {
        size_t room = sizeof(s_text) - s_capture.text_bytes - 1;
        /* Keep UTF-8 intact: omit a sentence rather than cut a code point. */
        if (n > room) s_capture.text_truncated = true;
        else if (n) {
            out = s_text + s_capture.text_bytes;
            s_capture.text_bytes += n;
            ++s_writers;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!out) return;
    memcpy(out, text, n);
    writer_done();
}

void tts_capture_stop(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_cloud_playing = false;
    check_locked(now_us);
    if (s_capture.phase == CAP_RECORDING) {
        s_capture.phase = CAP_SEALING;
        seal_locked();
    }
    portEXIT_CRITICAL(&s_state_lock);
}

void tts_capture_abort(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_cloud_playing = false;
    if (s_capture.phase == CAP_RECORDING || s_capture.phase == CAP_SEALING)
        fail_locked("channel_closed_before_tts_stop");
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t respond(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t control_handler(httpd_req_t *req)
{
    char body[65];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
        return respond(req, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid_body_length\"}");
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return respond(req, "408 Request Timeout", "{\"ok\":false,\"error\":\"read_failed\"}");
        received += (size_t)n;
    }
    body[received] = '\0';
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    bool arm = cJSON_IsString(action) && !strcmp(action->valuestring, "arm");
    bool cancel = cJSON_IsString(action) && !strcmp(action->valuestring, "cancel");
    cJSON_Delete(root);
    if (!arm && !cancel)
        return respond(req, "400 Bad Request", "{\"ok\":false,\"error\":\"action_must_be_arm_or_cancel\"}");
    /* Allocations happen only in the HTTP task, never in an audio callback. */
    uint8_t *allocated = NULL;
    /* HTTP handlers run serially on this server; only they allocate/free the
     * buffer. Audio callbacks reserve regions but never replace s_packets. */
    if (arm && !s_packets) {
        allocated = heap_caps_malloc(CAPTURE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!allocated) return respond(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"no_psram\"}");
    }
    int64_t now_us = esp_timer_get_time();
    uint8_t *released = NULL;
    portENTER_CRITICAL(&s_state_lock);
    check_locked(now_us);
    if (s_downloading || s_writers || (arm && (s_cloud_playing ||
            s_capture.phase == CAP_ARMED || s_capture.phase == CAP_RECORDING ||
            s_capture.phase == CAP_SEALING))) {
        portEXIT_CRITICAL(&s_state_lock);
        free(allocated);
        return respond(req, "409 Conflict", "{\"ok\":false,\"error\":\"busy_wait_for_reply_to_finish\"}");
    }
    uint32_t id = s_capture.id + 1;
    if (!id) id = 1;
    memset(&s_capture, 0, sizeof(s_capture));
    s_capture.id = id;
    if (arm) {
        if (allocated) s_packets = allocated;
        s_capture.phase = CAP_ARMED;
    } else {
        released = s_packets;
        s_packets = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);
    free(released);
    char reply[80];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"id\":%lu,\"state\":\"%s\"}",
             (unsigned long)id, arm ? "armed" : "idle");
    ESP_LOGI(TAG, "%s id=%lu (cloud audio only)", arm ? "Armed" : "Released", (unsigned long)id);
    return respond(req, "200 OK", reply);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    char query[96], id_text[16];
    bool download = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                    strstr(query, "download=1") != NULL;
    if (download) {
        if (httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK)
            return respond(req, "400 Bad Request", "{\"ok\":false,\"error\":\"capture_id_required\"}");
        char *end;
        unsigned long id = strtoul(id_text, &end, 10);
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_state_lock);
        check_locked(now_us);
        if (!id || *end || id != s_capture.id || s_capture.phase != CAP_COMPLETE || s_downloading || s_writers) {
            portEXIT_CRITICAL(&s_state_lock);
            return respond(req, "409 Conflict", "{\"ok\":false,\"error\":\"capture_not_complete_or_id_changed\"}");
        }
        s_downloading = true;
        size_t bytes = s_capture.bytes;
        uint32_t frames = s_capture.frames;
        const uint8_t *packets = s_packets;
        portEXIT_CRITICAL(&s_state_lock);
        /* Frozen until download finishes; no lock is held across network I/O.
         * FOP1 + u32 input-rate hint + u32 packet count + u32 payload bytes.
         * Payload: repeated little-endian u16 length followed by one Opus packet. */
        uint8_t header[16] = {'F', 'O', 'P', '1'};
        put_u32(header + 4, 16000);
        put_u32(header + 8, frames);
        put_u32(header + 12, (uint32_t)bytes);
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t ret = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
        for (size_t pos = 0; ret == ESP_OK && pos < bytes; pos += 2048) {
            size_t n = bytes - pos;
            if (n > 2048) n = 2048;
            ret = httpd_resp_send_chunk(req, (const char *)packets + pos, n);
        }
        if (ret == ESP_OK) ret = httpd_resp_send_chunk(req, NULL, 0);
        portENTER_CRITICAL(&s_state_lock);
        s_downloading = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ret;
    }
    /* Avoid putting the 2KB transcript on the shared HTTP task's small stack. */
    snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) return respond(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"no_memory\"}");
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    check_locked(now_us);
    snapshot->capture = s_capture;
    portEXIT_CRITICAL(&s_state_lock);
    capture_t *meta = &snapshot->capture;
    /* While recording, writers may append text. Expose it only after STOP
     * and all copies have finished. COMPLETE is immutable until an HTTP
     * control request; those requests cannot overlap this HTTP handler. */
    snapshot->text[0] = '\0';
    if (meta->phase == CAP_COMPLETE) {
        memcpy(snapshot->text, s_text, meta->text_bytes);
        snapshot->text[meta->text_bytes] = '\0';
    }
    static const char *states[] = {"idle", "armed", "recording", "sealing", "complete", "error"};
    cJSON *json = cJSON_CreateObject();
    if (!json) { free(snapshot); return respond(req, "503 Service Unavailable", "{\"ok\":false}"); }
    cJSON_AddBoolToObject(json, "ok", meta->phase != CAP_ERROR);
    cJSON_AddStringToObject(json, "state", states[meta->phase]);
    cJSON_AddNumberToObject(json, "id", meta->id);
    cJSON_AddNumberToObject(json, "frames", meta->frames);
    cJSON_AddNumberToObject(json, "bytes", meta->bytes);
    cJSON_AddNumberToObject(json, "capacity", CAPTURE_CAPACITY);
    cJSON_AddStringToObject(json, "cloud_text", snapshot->text);
    cJSON_AddBoolToObject(json, "text_truncated", meta->text_truncated);
    cJSON_AddStringToObject(json, "error", meta->error ? meta->error : "");
    free(snapshot);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = respond(req, text ? "200 OK" : "503 Service Unavailable", text ? text : "{\"ok\":false}");
    free(text);
    return ret;
}

esp_err_t tts_capture_http_init(void)
{
    const httpd_uri_t get_uri = {.uri = "/api/tts/capture", .method = HTTP_GET, .handler = capture_handler};
    const httpd_uri_t post_uri = {.uri = "/api/tts/capture", .method = HTTP_POST, .handler = control_handler};
    esp_err_t ret = ws_manager_server_register_uri(&get_uri);
    if (ret == ESP_OK) ret = ws_manager_server_register_uri(&post_uri);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Capture API ready; disabled until armed; 512KiB PSRAM on demand");
    return ret;
}
