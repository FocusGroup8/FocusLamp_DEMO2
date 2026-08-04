#include "voice_control_module_config.h"

#if (VOICE_CONTROL_MODULE_ENABLE == 1)

#include <stdio.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_driver.h"
#include "audio_service_module.h"
#include "lcd_module.h"
#if (LVGL_UI_ENABLE == 1)
#include "lvgl_ui.h"
#endif
#include "voice_control_module.h"

// ESP-SR Headers
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"

static const char* TAG = "voice_control_offline";

// 离线指令定义
#define CMD_ID_SWITCH_PAGE 1
#define CMD_ID_EXPR_HAPPY 2
#define CMD_ID_EXPR_NORMAL 3
#define CMD_ID_EXPR_SLEEPY 4
#define CMD_ID_BLINK_ON 5
#define CMD_ID_BLINK_OFF 6

#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#define VOICE_CONTROL_MULTINET_MODEL "mn7_cn"
#else
#define VOICE_CONTROL_MULTINET_MODEL "mn6_cn"
#endif

static voice_control_module_state_t  s_state             = VOICE_CONTROL_STATE_UNINIT;
static TaskHandle_t                  s_voice_task_handle = NULL;
static bool                          s_run_task          = false;
static bool                          s_audio_initialized = false;
static voice_control_module_config_t s_config            = VOICE_CONTROL_MODULE_DEFAULT_CONFIG();

static esp_err_t voice_control_next_page(void)
{
#if (LVGL_UI_ENABLE == 1)
    return lvgl_ui_next_page();
#else
    return lcd_module_next_page();
#endif
}

static esp_err_t voice_control_set_expression_normal(void)
{
#if (LVGL_UI_ENABLE == 1)
    return lvgl_ui_set_expression(LVGL_UI_EXPR_NORMAL);
#else
    return lcd_module_set_expression(LCD_EXPRESSION_NORMAL);
#endif
}

static esp_err_t voice_control_set_expression_happy(void)
{
#if (LVGL_UI_ENABLE == 1)
    return lvgl_ui_set_expression(LVGL_UI_EXPR_HAPPY);
#else
    return lcd_module_set_expression(LCD_EXPRESSION_HAPPY);
#endif
}

static esp_err_t voice_control_set_expression_sleepy(void)
{
#if (LVGL_UI_ENABLE == 1)
    return lvgl_ui_set_expression(LVGL_UI_EXPR_SLEEPY);
#else
    return lcd_module_set_expression(LCD_EXPRESSION_SLEEPY);
#endif
}

static void voice_control_set_auto_blink(bool enable)
{
#if (LVGL_UI_ENABLE == 1)
    lvgl_ui_set_auto_blink(enable);
#else
    lcd_module_set_auto_blink(enable);
#endif
}

// 模拟音频读取接口
extern esp_err_t audio_i2s_read(void* buffer, size_t buffer_size, size_t* bytes_read,
                                uint32_t timeout_ms);

static void voice_control_task(void* arg)
{
    ESP_LOGI(TAG, "--- Voice Control Task Started ---");
    ESP_LOGI(TAG, "Free heap before init: %lu bytes", esp_get_free_heap_size());

    // 1. 初始化 AFE (音频前端)
    ESP_LOGI(TAG, "Step 1: Initializing srmodel...");
    srmodel_list_t* models = esp_srmodel_init("model");
    if (!models)
    {
        ESP_LOGE(TAG, "Failed to init srmodel. Please check if 'model' partition exists and data "
                      "is flashed.");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "srmodel initialized. Free heap: %lu", esp_get_free_heap_size());

    afe_config_t* afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->wakenet_init = true;
    afe_config->aec_init     = false;

    ESP_LOGI(TAG, "Step 2: Getting AFE handle...");
    const esp_afe_sr_iface_t* afe_handle = esp_afe_handle_from_config(afe_config);
    if (!afe_handle)
    {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Step 3: Creating AFE data...");
    esp_afe_sr_data_t* afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data)
    {
        ESP_LOGE(TAG, "Failed to create AFE data (Likely insufficient memory or model error)");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AFE data created. Free heap: %lu", esp_get_free_heap_size());

    // 2. 初始化离线指令 (MultiNet)
    ESP_LOGI(TAG, "Step 4: Initializing MultiNet (Chinese): %s", VOICE_CONTROL_MULTINET_MODEL);
    const esp_mn_iface_t* multinet = esp_mn_handle_from_name(VOICE_CONTROL_MULTINET_MODEL);
    if (!multinet)
    {
        ESP_LOGE(TAG, "Failed to get multinet handle for %s. Check menuconfig model selection.",
                 VOICE_CONTROL_MULTINET_MODEL);
        afe_handle->destroy(afe_data);
        s_voice_task_handle = NULL;
        s_state             = VOICE_CONTROL_STATE_ERROR;
        vTaskDelete(NULL);
        return;
    }

    model_iface_data_t* mn_data = multinet->create(VOICE_CONTROL_MULTINET_MODEL, 6000);
    if (!mn_data)
    {
        ESP_LOGE(TAG, "Failed to create MultiNet data for %s", VOICE_CONTROL_MULTINET_MODEL);
        afe_handle->destroy(afe_data);
        s_voice_task_handle = NULL;
        s_state             = VOICE_CONTROL_STATE_ERROR;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MultiNet data created. Free heap: %lu", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Step 5: Allocating commands...");
    esp_mn_commands_alloc(multinet, mn_data);
    esp_mn_commands_add(CMD_ID_SWITCH_PAGE, "qie huan ye mian");
    esp_mn_commands_add(CMD_ID_EXPR_HAPPY, "xian shi kai xin");
    esp_mn_commands_add(CMD_ID_EXPR_NORMAL, "xian shi zheng chang");
    esp_mn_commands_add(CMD_ID_EXPR_SLEEPY, "xian shi shui mian");
    esp_mn_commands_add(CMD_ID_BLINK_ON, "kai qi zha yan");
    esp_mn_commands_add(CMD_ID_BLINK_OFF, "guan bi zha yan");
    esp_mn_commands_update();
    ESP_LOGI(TAG, "Commands updated. Say Chinese: 切换页面 / 显示开心 / 显示正常 / 显示睡眠 / "
                  "开启眨眼 / 关闭眨眼");
    ESP_LOGI(TAG, "Commands updated. Free heap: %lu", esp_get_free_heap_size());

    int      chunk_size  = afe_handle->get_feed_chunksize(afe_data);
    int16_t* buffer      = malloc(chunk_size * sizeof(int16_t));
    int32_t* temp_buffer = malloc(chunk_size * sizeof(int32_t));

    if (!buffer || !temp_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffers (Insufficient memory)");
        if (buffer)
            free(buffer);
        if (temp_buffer)
            free(temp_buffer);
        multinet->destroy(mn_data);
        afe_handle->destroy(afe_data);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Step 6: Voice Task Ready! Listening for wake word: %s", s_config.wake_word);
    s_state = VOICE_CONTROL_STATE_LISTENING;

    int debug_count = 0;
    while (s_run_task)
    {
        size_t    bytes_read = 0;
        esp_err_t ret =
            audio_i2s_read(temp_buffer, chunk_size * sizeof(int32_t), &bytes_read, portMAX_DELAY);

        if (ret != ESP_OK || bytes_read == 0)
            continue;

        // 32位 转换为 16位 PCM
        int samples = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples; i++)
        {
            int32_t raw_value = temp_buffer[i];
            int32_t value     = raw_value >> 12;
            buffer[i] = (value > 32767) ? 32767 : (value < -32768) ? -32768 : (int16_t)value;
        }

        // 调试输出：每 50 帧输出一次音频状态
        debug_count++;
        if (debug_count % 50 == 0)
        {
            int16_t max_val = 0;
            int32_t avg_val = 0;
            for (int i = 0; i < samples; i++)
            {
                int16_t val = buffer[i];
                if (val < 0)
                    val = -val;
                if (val > max_val)
                    max_val = val;
                avg_val += val;
            }
            avg_val /= (samples > 0 ? samples : 1);
            ESP_LOGI(TAG, "[Debug] Mic Power - Max: %d, Avg: %ld, Heap: %lu", max_val, avg_val,
                     esp_get_free_heap_size());
        }

        afe_handle->feed(afe_data, buffer);

        afe_fetch_result_t* res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL)
            continue;

        if (res->wakeup_state == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, "Wake word detected!");
        }

        esp_mn_state_t mn_state = multinet->detect(mn_data, res->data);
        if (mn_state == ESP_MN_STATE_DETECTED)
        {
            esp_mn_results_t* mn_res = multinet->get_results(mn_data);
            for (int i = 0; i < mn_res->num; i++)
            {
                int command_id = mn_res->command_id[i];
                ESP_LOGI(TAG, "Command detected ID: %d", command_id);

                switch (command_id)
                {
                case CMD_ID_SWITCH_PAGE:
                    ESP_LOGI(TAG, ">> Action: Next Page");
                    ESP_LOGI(TAG, "Page switch result: %s",
                             esp_err_to_name(voice_control_next_page()));
                    break;
                case CMD_ID_EXPR_HAPPY:
                    ESP_LOGI(TAG, ">> Action: Happy Expression");
                    ESP_LOGI(TAG, "Set happy result: %s",
                             esp_err_to_name(voice_control_set_expression_happy()));
                    break;
                case CMD_ID_EXPR_NORMAL:
                    ESP_LOGI(TAG, ">> Action: Normal Expression");
                    ESP_LOGI(TAG, "Set normal result: %s",
                             esp_err_to_name(voice_control_set_expression_normal()));
                    break;
                case CMD_ID_EXPR_SLEEPY:
                    ESP_LOGI(TAG, ">> Action: Sleepy Expression");
                    ESP_LOGI(TAG, "Set sleepy result: %s",
                             esp_err_to_name(voice_control_set_expression_sleepy()));
                    break;
                case CMD_ID_BLINK_ON:
                    ESP_LOGI(TAG, ">> Action: Blink ON");
                    voice_control_set_auto_blink(true);
                    break;
                case CMD_ID_BLINK_OFF:
                    ESP_LOGI(TAG, ">> Action: Blink OFF");
                    voice_control_set_auto_blink(false);
                    break;
                }
            }
        }
    }

    free(buffer);
    if (temp_buffer)
        free(temp_buffer);
    if (mn_data)
        multinet->destroy(mn_data);
    afe_handle->destroy(afe_data);
    esp_mn_commands_free();
    s_voice_task_handle = NULL;
    if (s_state != VOICE_CONTROL_STATE_UNINIT)
    {
        s_state = VOICE_CONTROL_STATE_IDLE;
    }
    vTaskDelete(NULL);
}

esp_err_t voice_control_module_init(const voice_control_module_config_t* config)
{
    if (s_state != VOICE_CONTROL_STATE_UNINIT)
        return ESP_ERR_INVALID_STATE;
    s_config =
        config ? *config : (voice_control_module_config_t)VOICE_CONTROL_MODULE_DEFAULT_CONFIG();
    audio_driver_config_t audio_config = {
        .sample_rate            = s_config.audio_sample_rate,
        .i2s_num                = AUDIO_DRIVER_I2S_NUM,
        .bclk_gpio              = AUDIO_DRIVER_BCLK_GPIO,
        .ws_gpio                = AUDIO_DRIVER_WS_GPIO,
        .dout_gpio              = AUDIO_DRIVER_DOUT_GPIO,
        .din_gpio               = AUDIO_DRIVER_DIN_GPIO,
        .volume                 = AUDIO_DRIVER_DEFAULT_VOLUME,
        .gain                   = AUDIO_DRIVER_DEFAULT_GAIN,
        .enable_noise_reduction = AUDIO_DRIVER_ENABLE_NOISE_REDUCTION,
        .noise_threshold        = AUDIO_DRIVER_NOISE_THRESHOLD,
    };
    esp_err_t ret = audio_driver_init(&audio_config);
    if (ret != ESP_OK)
        return ret;
    s_audio_initialized = true;
    s_state             = VOICE_CONTROL_STATE_IDLE;
    ESP_LOGI(TAG, "Voice control module initialized");
    return ESP_OK;
}

esp_err_t voice_control_module_start(void)
{
    if (s_state != VOICE_CONTROL_STATE_IDLE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t ret = audio_driver_start_recording();
    if (ret != ESP_OK)
        return ret;
    s_run_task = true;
    if (xTaskCreate(voice_control_task, "voice_control_task", VOICE_CONTROL_TASK_STACK_SIZE, NULL,
                    VOICE_CONTROL_TASK_PRIORITY, &s_voice_task_handle) != pdPASS)
    {
        s_run_task = false;
        audio_driver_stop_recording();
        return ESP_ERR_NO_MEM;
    }
    s_state = VOICE_CONTROL_STATE_CONNECTING;
    return ESP_OK;
}

esp_err_t voice_control_module_stop(void)
{
    if (s_state == VOICE_CONTROL_STATE_UNINIT)
        return ESP_ERR_INVALID_STATE;
    s_run_task = false;
    if (s_audio_initialized)
    {
        audio_driver_stop_recording();
    }
    s_state = VOICE_CONTROL_STATE_IDLE;
    return ESP_OK;
}

voice_control_module_state_t voice_control_module_get_state(void)
{
    return s_state;
}

esp_err_t voice_control_module_deinit(void)
{
    if (s_state == VOICE_CONTROL_STATE_UNINIT)
        return ESP_OK;
    voice_control_module_stop();
    if (s_audio_initialized)
    {
        audio_driver_deinit();
        s_audio_initialized = false;
    }
    s_state = VOICE_CONTROL_STATE_UNINIT;
    return ESP_OK;
}

esp_err_t voice_control_module_register_controller(const module_controller_t* controller)
{
    (void)controller;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_control_module_unregister_controller(const char* name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // VOICE_CONTROL_MODULE_ENABLE