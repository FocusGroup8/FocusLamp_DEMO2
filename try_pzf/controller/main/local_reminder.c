#include "local_reminder.h"
#include "local_welcome_policy.h"
#include "audio_bridge.h"
#include "wake_word_engine.h"
#include "xiaozhi_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

extern const uint8_t clip_start[] asm("_binary_sedentary_reminder_wav_start");
extern const uint8_t clip_end[] asm("_binary_sedentary_reminder_wav_end");
#define DECLARE_WELCOME(n) \
    extern const uint8_t welcome_##n##_start[] asm("_binary_welcome_" #n "_wav_start"); \
    extern const uint8_t welcome_##n##_end[] asm("_binary_welcome_" #n "_wav_end");
DECLARE_WELCOME(01)
DECLARE_WELCOME(02)
DECLARE_WELCOME(03)
DECLARE_WELCOME(04)
DECLARE_WELCOME(05)
DECLARE_WELCOME(06)
DECLARE_WELCOME(07)
DECLARE_WELCOME(08)
DECLARE_WELCOME(09)
#undef DECLARE_WELCOME

typedef struct {
    const uint8_t *start, *end;
    const char *text;
    const uint8_t *pcm;
    size_t pcm_size;
} clip_t;
static clip_t s_reminder = { .start = clip_start, .end = clip_end };
#define WELCOME_CLIP(n, message) { .start = welcome_##n##_start, .end = welcome_##n##_end, .text = message }
static clip_t s_welcomes[LOCAL_WELCOME_COUNT] = {
    WELCOME_CLIP(01, "嗨主人想我了吗？focus一直都在哦。"),
    WELCOME_CLIP(02, "哈哈，摸得我脑袋痒痒的，开心值又加一啦。"),
    WELCOME_CLIP(03, "欢迎回来，小灯已经就位随时陪你出发。"),
    WELCOME_CLIP(04, "今天辛苦了，休息一下，我会一直陪着你。"),
    WELCOME_CLIP(05, "给你一点光，也给你一点好心情。"),
    WELCOME_CLIP(06, "准备好了吗？我们一起专注把事情做好。"),
    WELCOME_CLIP(07, "收到摸摸头，今天也要元气满满哦。"),
    WELCOME_CLIP(08, "别着急，我们一件一件慢慢来。"),
    WELCOME_CLIP(09, "有你在我的小世界又亮起来了。"),
};
#undef WELCOME_CLIP

typedef struct {
    uint32_t cookie;
    local_reminder_allowed_t allowed;
    local_reminder_result_t result;
} request_t;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_data[sizeof(request_t)];
static portMUX_TYPE s_welcome_lock = portMUX_INITIALIZER_UNLOCKED;
static local_welcome_policy_t s_welcome;
static const char *TAG = "LOCAL_REMINDER";

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }

static esp_err_t read_clip(clip_t *clip)
{
    const uint8_t *start = clip->start;
    size_t size = (size_t)(clip->end - start);
    if (size < 12 || memcmp(start, "RIFF", 4) ||
        memcmp(start + 8, "WAVE", 4)) return ESP_ERR_INVALID_RESPONSE;
    bool format_ok = false;
    for (size_t pos = 12; pos + 8 <= size;) {
        size_t n = le32(start + pos + 4);
        const uint8_t *body = start + pos + 8;
        if (n > size - pos - 8) return ESP_ERR_INVALID_RESPONSE;
        if (!memcmp(start + pos, "fmt ", 4)) {
            if (n < 16) return ESP_ERR_INVALID_RESPONSE;
            format_ok = le16(body) == 1 && le16(body+2) == 1 &&
                le32(body+4) == 16000 && le16(body+12) == 2 && le16(body+14) == 16;
        } else if (!memcmp(start + pos, "data", 4)) {
            if (!format_ok || !n || (n & 1)) return ESP_ERR_INVALID_RESPONSE;
            clip->pcm = body;
            clip->pcm_size = n;
            return ESP_OK;
        }
        pos += 8 + n + (n & 1);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static bool welcome_cancelled(void *ctx)
{
    uint32_t generation = *(const uint32_t *)ctx;
    portENTER_CRITICAL(&s_welcome_lock);
    bool cancelled = local_welcome_policy_cancelled(&s_welcome, generation);
    portEXIT_CRITICAL(&s_welcome_lock);
    return cancelled;
}

static void resume_local_recognition(void)
{
    esp_err_t ret = wake_word_engine_resume_local_audio();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Recognition resume failed: %s", esp_err_to_name(ret));
}

static void play_welcome(uint32_t generation, uint8_t index)
{
    esp_err_t ret = xiaozhi_manager_local_audio_begin();
    bool admitted = ret == ESP_OK, paused = false, output_started = false;
    if (admitted && !welcome_cancelled(&generation)) {
        ret = wake_word_engine_pause_local_audio();
        paused = ret == ESP_OK;
        if (paused) {
            if (!welcome_cancelled(&generation)) {
                clip_t *clip = &s_welcomes[index];
                ESP_LOGI(TAG, "Welcome %u/%u playback requested: %s", (unsigned)index + 1,
                         LOCAL_WELCOME_COUNT, clip->text);
                ret = audio_bridge_play_local_pcm16_cancellable(clip->pcm, clip->pcm_size,
                    welcome_cancelled, &generation, &output_started);
            } else ret = ESP_ERR_INVALID_STATE;
        }
    } else if (admitted) ret = ESP_ERR_INVALID_STATE;
    bool cancelled = welcome_cancelled(&generation);
    if (paused) resume_local_recognition();
    if (admitted) xiaozhi_manager_local_audio_end();
    portENTER_CRITICAL(&s_welcome_lock);
    /* The bridge reports a successful PCM write, not merely lock admission.
     * Commit even if that output was subsequently cancelled; never skip a clip
     * cancelled before its first successful write or on TX-lock failure. */
    if (output_started) local_welcome_policy_output_started(&s_welcome, generation);
    local_welcome_policy_finish(&s_welcome, generation);
    portEXIT_CRITICAL(&s_welcome_lock);
    ESP_LOGI(TAG, "Welcome %u/%u %s: %s (pcm_started=%d)", (unsigned)index + 1,
             LOCAL_WELCOME_COUNT, cancelled ? "cancelled" : "output finished",
             esp_err_to_name(ret), output_started);
}

static void play_reminder(const request_t *req)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    bool admitted = false, paused = false;
    if (req->allowed(req->cookie)) {
        ret = xiaozhi_manager_local_audio_begin();
        admitted = ret == ESP_OK;
        if (admitted) {
            ret = wake_word_engine_pause_local_audio();
            paused = ret == ESP_OK;
            if (paused) {
                if (req->allowed(req->cookie)) {
                    ESP_LOGI(TAG, "cycle=%u local playback started", (unsigned)req->cookie);
                    ret = audio_bridge_play_local_pcm16(s_reminder.pcm, s_reminder.pcm_size);
                } else ret = ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (paused) resume_local_recognition();
    if (admitted) xiaozhi_manager_local_audio_end();
    ESP_LOGI(TAG, "cycle=%u local output result=%s (verify by listening)",
             (unsigned)req->cookie, esp_err_to_name(ret));
    req->result(req->cookie, ret);
}

static void clip_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t generation;
            uint8_t index;
            portENTER_CRITICAL(&s_welcome_lock);
            bool welcome = local_welcome_policy_take(&s_welcome, &generation, &index);
            portEXIT_CRITICAL(&s_welcome_lock);
            if (welcome) {
                play_welcome(generation, index);
                continue;
            }
            request_t req;
            if (xQueueReceive(s_queue, &req, 0) != pdTRUE) break;
            play_reminder(&req);
        }
    }
}

esp_err_t local_reminder_start(void)
{
    if (s_task) return ESP_OK;
    esp_err_t ret = read_clip(&s_reminder);
    if (ret != ESP_OK) return ret;
    for (size_t i = 0; i < LOCAL_WELCOME_COUNT; ++i) {
        ret = read_clip(&s_welcomes[i]);
        if (ret != ESP_OK) return ret;
    }
    s_queue = xQueueCreateStatic(1, sizeof(request_t), s_queue_data, &s_queue_storage);
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(clip_task, "local_reminder", 6144, NULL, 3, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Embedded PCM16 ready: reminder + %u welcomes, 16kHz mono",
             LOCAL_WELCOME_COUNT);
    return ESP_OK;
}

esp_err_t local_reminder_submit(uint32_t cookie, local_reminder_allowed_t allowed,
                                local_reminder_result_t result)
{
    if (!s_task || !allowed || !result) return ESP_ERR_INVALID_STATE;
    request_t req = {cookie, allowed, result};
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool local_welcome_matches(const char *text)
{
    /* These are triggers, not spoken content. Keep other text on cloud TTS. */
    static const char *aliases[] = {
        "请只说：欢迎回来，我是小智，今天也一起专注学习吧！",
        "请只说：你好呀，准备好了吗？让我们开始今天的学习吧！",
        "请只说：很高兴见到你，记得保持坐姿，也别忘了适时休息！",
    };
    if (!text) return false;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        if (!strcmp(text, aliases[i])) return true;
    return false;
}

esp_err_t local_welcome_submit(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_welcome_lock);
    local_welcome_policy_request(&s_welcome);
    portEXIT_CRITICAL(&s_welcome_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void local_welcome_cancel(void)
{
    portENTER_CRITICAL(&s_welcome_lock);
    local_welcome_policy_cancel(&s_welcome);
    portEXIT_CRITICAL(&s_welcome_lock);
    if (s_task) xTaskNotifyGive(s_task);
}
