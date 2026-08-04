/*
 * demo_fake_detection.c - Demo fake detection implementation
 *
 * 场景4: 假玩手机/电脑检测 - 定时触发TTS播报
 * 场景5: 假久坐提醒 - 定时触发TTS播报
 *
 * 通过周期定时器模拟检测事件，通过 tts_bridge 发送播报文字到 TTS 板播放。
 */

#include "demo_fake_detection.h"
#include "event_bus.h"
#include "event_def.h"
#include "tts_bridge.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fake_detect";

/* 定时器间隔（微秒）*/
#define PHONE_DETECT_INTERVAL_US  (120 * 1000000ULL)  /* 120秒 */
#define SEDENTARY_INTERVAL_US     (180 * 1000000ULL)  /* 180秒 */

/* 玩手机/电脑检测播报内容（交替使用）*/
static const char *s_phone_messages[] = {
    "识别到您正在玩手机，快把手机放下，好好专注把工作完成吧",
    "识别到您正在玩电脑游戏，快停下来活动一下，然后先专注把工作完成再玩吧~",
};
#define PHONE_MSG_COUNT 2

/* 久坐提醒播报内容 */
static const char *s_sedentary_message = "你已经坐了很久，请站起来活动活动吧~";

static esp_timer_handle_t s_phone_timer = NULL;
static esp_timer_handle_t s_sedentary_timer = NULL;
static int s_phone_msg_idx = 0;
static bool s_initialized = false;

/*---------------------------------------------------------------
 * Timer callbacks
 *-------------------------------------------------------------*/
static void phone_detect_callback(void *arg)
{
    (void)arg;
    const char *msg = s_phone_messages[s_phone_msg_idx % PHONE_MSG_COUNT];
    s_phone_msg_idx++;
    ESP_LOGI(TAG, "Phone detection triggered: %s", msg);
    tts_bridge_speak(msg);
}

static void sedentary_callback(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Sedentary reminder triggered: %s", s_sedentary_message);
    tts_bridge_speak(s_sedentary_message);
}

/*---------------------------------------------------------------
 * Startup event handler
 *-------------------------------------------------------------*/
static void on_startup_complete(event_t *event, void *context)
{
    (void)event;
    (void)context;
    ESP_LOGI(TAG, "System startup complete, starting fake detection timers");

    /* 启动玩手机检测定时器 */
    if (s_phone_timer) {
        esp_timer_start_periodic(s_phone_timer, PHONE_DETECT_INTERVAL_US);
    }

    /* 启动久坐提醒定时器 */
    if (s_sedentary_timer) {
        esp_timer_start_periodic(s_sedentary_timer, SEDENTARY_INTERVAL_US);
    }
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t demo_fake_detection_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret;

    /* 创建玩手机检测定时器 */
    const esp_timer_create_args_t phone_timer_args = {
        .callback = phone_detect_callback,
        .arg = NULL,
        .name = "fake_phone",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&phone_timer_args, &s_phone_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create phone timer: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建久坐提醒定时器 */
    const esp_timer_create_args_t sedentary_timer_args = {
        .callback = sedentary_callback,
        .arg = NULL,
        .name = "fake_sedentary",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&sedentary_timer_args, &s_sedentary_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create sedentary timer: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 订阅系统启动完成事件，启动后才开始定时器 */
    ret = event_bus_subscribe(EV_SYS_STARTUP_COMPLETE, on_startup_complete, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_SYS_STARTUP_COMPLETE: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Demo fake detection initialized (phone=%llds, sedentary=%llds)",
             (long long)(PHONE_DETECT_INTERVAL_US / 1000000),
             (long long)(SEDENTARY_INTERVAL_US / 1000000));
    return ESP_OK;
}
