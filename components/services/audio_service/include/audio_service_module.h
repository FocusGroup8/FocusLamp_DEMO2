#ifndef AUDIO_SERVICE_MODULE_H
#define AUDIO_SERVICE_MODULE_H

#include "esp_err.h"

#include "audio_service_module_config.h"
#include "audio_service_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (AUDIO_SERVICE_MODULE_ENABLE == 1)

    esp_err_t audio_service_module_init(const audio_service_module_config_t* config);
    esp_err_t audio_service_module_deinit(void);
    esp_err_t audio_service_module_start(void);
    esp_err_t audio_service_module_stop(void);
    esp_err_t audio_service_module_push_opus_packet(const uint8_t* packet, size_t len);
    esp_err_t audio_service_module_pop_opus_packet(uint8_t* packet, size_t len, size_t* actual_len);
    esp_err_t audio_service_module_set_callbacks(const audio_service_module_callbacks_t* callbacks);
    esp_err_t audio_service_module_enable_wake_word(bool enable);
    esp_err_t audio_service_module_enable_voice_processing(bool enable);
    bool audio_service_module_is_voice_detected(void);
    bool audio_service_module_is_wake_word_detected(void);
    audio_service_module_state_t audio_service_module_get_state(void);
    esp_err_t audio_service_module_print_config(void);
    esp_err_t audio_service_module_set_volume(float volume);
    esp_err_t audio_service_module_set_gain(float gain);
    float audio_service_module_get_volume(void);
    float audio_service_module_get_gain(void);
    esp_err_t audio_service_module_set_mute(bool mute);
    bool audio_service_module_get_mute(void);

#else

static inline esp_err_t audio_service_module_init(const audio_service_module_config_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_push_opus_packet(const uint8_t* packet, size_t len)
{
    (void)packet; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_pop_opus_packet(uint8_t* packet, size_t len,
                                                             size_t* actual_len)
{
    (void)packet; (void)len; (void)actual_len;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t
audio_service_module_set_callbacks(const audio_service_module_callbacks_t* callbacks)
{
    (void)callbacks;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_enable_wake_word(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_enable_voice_processing(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool audio_service_module_is_voice_detected(void)
{
    return false;
}
static inline bool audio_service_module_is_wake_word_detected(void)
{
    return false;
}
static inline audio_service_module_state_t audio_service_module_get_state(void)
{
    return AUDIO_SERVICE_STATE_UNINIT;
}
static inline esp_err_t audio_service_module_print_config(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_set_volume(float volume)
{
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_service_module_set_gain(float gain)
{
    (void)gain;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline float audio_service_module_get_volume(void)
{
    return 0.0f;
}
static inline float audio_service_module_get_gain(void)
{
    return 0.0f;
}
static inline esp_err_t audio_service_module_set_mute(bool mute)
{
    (void)mute;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool audio_service_module_get_mute(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SERVICE_MODULE_H */
