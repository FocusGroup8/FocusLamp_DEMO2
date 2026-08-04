#include "audio_processor.h"

#include <stdlib.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char* TAG = "audio_processor";

#define PROCESSOR_RUNNING_BIT (1 << 0)

struct audio_processor_t
{
    const esp_afe_sr_iface_t* afe_iface;
    esp_afe_sr_data_t*        afe_data;
    srmodel_list_t*           models;

    audio_processor_output_callback_t    output_callback;
    audio_processor_vad_callback_t       vad_callback;
    audio_processor_wake_word_callback_t wake_word_callback;
    void*                                user_data;

    EventGroupHandle_t event_group;
    TaskHandle_t       task_handle;

    int feed_chunksize;
    int feed_channels;
    int mic_channels;
    int ref_channels;

    audio_processor_vad_state_t last_vad_state;
    bool                        running;
};

static void audio_processor_task(void* arg)
{
    audio_processor_t* processor = (audio_processor_t*)arg;

    ESP_LOGI(TAG, "Audio processor task started");

    while (processor->running)
    {
        EventBits_t bits = xEventGroupWaitBits(processor->event_group, PROCESSOR_RUNNING_BIT,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(100));
        if ((bits & PROCESSOR_RUNNING_BIT) == 0)
        {
            continue;
        }

        afe_fetch_result_t* res =
            processor->afe_iface->fetch_with_delay(processor->afe_data, pdMS_TO_TICKS(100));
        if (res == NULL || res->ret_value == ESP_FAIL)
        {
            continue;
        }

        if (processor->vad_callback)
        {
            audio_processor_vad_state_t vad_state = (res->vad_state == VAD_SPEECH)
                                                        ? AUDIO_PROCESSOR_VAD_SPEECH
                                                        : AUDIO_PROCESSOR_VAD_SILENCE;
            if (vad_state != processor->last_vad_state)
            {
                processor->last_vad_state = vad_state;
                processor->vad_callback(vad_state, processor->user_data);
            }
        }

        if (processor->wake_word_callback && res->wakeup_state == WAKENET_DETECTED)
        {
            processor->wake_word_callback(res->wake_word_index, NULL, processor->user_data);
        }

        if (processor->output_callback && res->data && res->data_size > 0)
        {
            processor->output_callback(res->data, res->data_size, processor->user_data);
        }
    }

    ESP_LOGI(TAG, "Audio processor task stopped");
    vTaskDelete(NULL);
}

audio_processor_t* audio_processor_create(const audio_processor_config_t* config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }

    audio_processor_t* processor = calloc(1, sizeof(audio_processor_t));
    if (processor == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate processor");
        return NULL;
    }

    processor->event_group = xEventGroupCreate();
    if (processor->event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        free(processor);
        return NULL;
    }

    processor->models = esp_srmodel_init("model");
    if (processor->models == NULL)
    {
        ESP_LOGE(TAG, "Failed to init models");
        vEventGroupDelete(processor->event_group);
        free(processor);
        return NULL;
    }

    char* input_format = malloc(config->mic_channels + config->ref_channels + 1);
    if (input_format == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate input format");
        esp_srmodel_deinit(processor->models);
        vEventGroupDelete(processor->event_group);
        free(processor);
        return NULL;
    }

    int pos = 0;
    for (int i = 0; i < config->mic_channels; i++)
    {
        input_format[pos++] = 'M';
    }
    for (int i = 0; i < config->ref_channels; i++)
    {
        input_format[pos++] = 'R';
    }
    input_format[pos] = '\0';

    ESP_LOGI(TAG, "Input format: %s", input_format);

    // Use LOW_COST mode for better compatibility with ESP32-P4
    // HIGH_PERF mode may cause crash during model initialization on some chips
    afe_config_t* afe_config =
        afe_config_init(input_format, processor->models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL)
    {
        ESP_LOGE(TAG, "Failed to init AFE config");
        free(input_format);
        esp_srmodel_deinit(processor->models);
        vEventGroupDelete(processor->event_group);
        free(processor);
        return NULL;
    }

    afe_config->aec_init     = config->enable_aec;
    afe_config->vad_init     = config->enable_vad;
    afe_config->wakenet_init = config->enable_wake_word;

    if (config->enable_wake_word && config->wake_word_model_name)
    {
        afe_config->wakenet_model_name = (char*)config->wake_word_model_name;
    }

    processor->afe_iface = esp_afe_handle_from_config(afe_config);
    if (processor->afe_iface == NULL)
    {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        free(input_format);
        esp_srmodel_deinit(processor->models);
        vEventGroupDelete(processor->event_group);
        free(processor);
        return NULL;
    }

    processor->afe_data = processor->afe_iface->create_from_config(afe_config);
    if (processor->afe_data == NULL)
    {
        ESP_LOGE(TAG, "Failed to create AFE data");
        free(input_format);
        esp_srmodel_deinit(processor->models);
        vEventGroupDelete(processor->event_group);
        free(processor);
        return NULL;
    }

    processor->feed_chunksize = processor->afe_iface->get_feed_chunksize(processor->afe_data);
    processor->feed_channels  = processor->afe_iface->get_channel_num(processor->afe_data);
    processor->mic_channels   = config->mic_channels;
    processor->ref_channels   = config->ref_channels;
    processor->last_vad_state = AUDIO_PROCESSOR_VAD_SILENCE;
    processor->running        = false;

    ESP_LOGI(TAG, "Audio processor created: feed_chunksize=%d, feed_channels=%d",
             processor->feed_chunksize, processor->feed_channels);

    free(input_format);
    return processor;
}

void audio_processor_destroy(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return;
    }

    processor->running = false;
    if (processor->task_handle)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (processor->afe_data)
    {
        processor->afe_iface->destroy(processor->afe_data);
    }

    if (processor->models)
    {
        esp_srmodel_deinit(processor->models);
    }

    if (processor->event_group)
    {
        vEventGroupDelete(processor->event_group);
    }

    free(processor);
}

esp_err_t audio_processor_feed(audio_processor_t* processor, const int16_t* mic_data, int mic_size,
                               const int16_t* ref_data, int ref_size)
{
    if (processor == NULL || processor->afe_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int      total_size = mic_size + ref_size;
    int16_t* feed_data  = malloc(total_size * sizeof(int16_t));
    if (feed_data == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    int pos = 0;
    for (int i = 0; i < mic_size; i++)
    {
        feed_data[pos++] = mic_data[i];
    }
    for (int i = 0; i < ref_size; i++)
    {
        feed_data[pos++] = ref_data[i];
    }

    processor->afe_iface->feed(processor->afe_data, feed_data);

    free(feed_data);
    return ESP_OK;
}

void audio_processor_set_output_callback(audio_processor_t*                processor,
                                         audio_processor_output_callback_t callback,
                                         void*                             user_data)
{
    if (processor)
    {
        processor->output_callback = callback;
        processor->user_data       = user_data;
    }
}

void audio_processor_set_vad_callback(audio_processor_t*             processor,
                                      audio_processor_vad_callback_t callback, void* user_data)
{
    if (processor)
    {
        processor->vad_callback = callback;
        processor->user_data    = user_data;
    }
}

void audio_processor_set_wake_word_callback(audio_processor_t*                   processor,
                                            audio_processor_wake_word_callback_t callback,
                                            void*                                user_data)
{
    if (processor)
    {
        processor->wake_word_callback = callback;
        processor->user_data          = user_data;
    }
}

int audio_processor_get_feed_chunksize(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return 0;
    }
    return processor->feed_chunksize;
}

int audio_processor_get_channel_num(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return 0;
    }
    return processor->feed_channels;
}

esp_err_t audio_processor_start(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (processor->running)
    {
        return ESP_OK;
    }

    processor->running = true;

    BaseType_t ret = xTaskCreate(audio_processor_task, "audio_processor", 8192, processor, 5,
                                 &processor->task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task");
        processor->running = false;
        return ESP_FAIL;
    }

    xEventGroupSetBits(processor->event_group, PROCESSOR_RUNNING_BIT);
    ESP_LOGI(TAG, "Audio processor started");
    return ESP_OK;
}

esp_err_t audio_processor_stop(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!processor->running)
    {
        return ESP_OK;
    }

    xEventGroupClearBits(processor->event_group, PROCESSOR_RUNNING_BIT);
    processor->running = false;

    if (processor->afe_iface && processor->afe_data)
    {
        processor->afe_iface->reset_buffer(processor->afe_data);
    }

    ESP_LOGI(TAG, "Audio processor stopped");
    return ESP_OK;
}

esp_err_t audio_processor_reset(audio_processor_t* processor)
{
    if (processor == NULL || processor->afe_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Reset AFE internal buffer (required after wake word detection)
    // This clears the internal state and allows new wake word detection
    if (processor->afe_iface && processor->afe_iface->reset_buffer)
    {
        processor->afe_iface->reset_buffer(processor->afe_data);
        ESP_LOGD(TAG, "Audio processor buffer reset");
    }

    return ESP_OK;
}

bool audio_processor_is_running(audio_processor_t* processor)
{
    if (processor == NULL)
    {
        return false;
    }
    return processor->running;
}
