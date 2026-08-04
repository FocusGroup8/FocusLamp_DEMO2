#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include "esp_afe_sr_models.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct audio_processor_t audio_processor_t;

    typedef enum
    {
        AUDIO_PROCESSOR_VAD_SILENCE = 0,
        AUDIO_PROCESSOR_VAD_SPEECH  = 1,
    } audio_processor_vad_state_t;

    typedef enum
    {
        AUDIO_PROCESSOR_WAKE_WORD_NONE     = 0,
        AUDIO_PROCESSOR_WAKE_WORD_DETECTED = 1,
    } audio_processor_wake_word_state_t;

    typedef struct
    {
        bool        enable_aec;
        bool        enable_vad;
        bool        enable_wake_word;
        const char* wake_word_model_name;
        int         mic_channels;
        int         ref_channels;
        int         sample_rate;
    } audio_processor_config_t;

    typedef void (*audio_processor_output_callback_t)(const int16_t* data, int data_size,
                                                      void* user_data);
    typedef void (*audio_processor_vad_callback_t)(audio_processor_vad_state_t vad_state,
                                                   void*                       user_data);
    typedef void (*audio_processor_wake_word_callback_t)(int         wake_word_index,
                                                         const char* wake_word_name,
                                                         void*       user_data);

    audio_processor_t* audio_processor_create(const audio_processor_config_t* config);
    void               audio_processor_destroy(audio_processor_t* processor);

    esp_err_t audio_processor_feed(audio_processor_t* processor, const int16_t* mic_data,
                                   int mic_size, const int16_t* ref_data, int ref_size);

    void audio_processor_set_output_callback(audio_processor_t*                processor,
                                             audio_processor_output_callback_t callback,
                                             void*                             user_data);
    void audio_processor_set_vad_callback(audio_processor_t*             processor,
                                          audio_processor_vad_callback_t callback, void* user_data);
    void audio_processor_set_wake_word_callback(audio_processor_t*                   processor,
                                                audio_processor_wake_word_callback_t callback,
                                                void*                                user_data);

    int audio_processor_get_feed_chunksize(audio_processor_t* processor);
    int audio_processor_get_channel_num(audio_processor_t* processor);

    esp_err_t audio_processor_start(audio_processor_t* processor);
    esp_err_t audio_processor_stop(audio_processor_t* processor);
    esp_err_t audio_processor_reset(audio_processor_t* processor);
    bool      audio_processor_is_running(audio_processor_t* processor);

#ifdef __cplusplus
}
#endif

#endif
