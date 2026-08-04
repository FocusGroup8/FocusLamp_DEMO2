/*
 * audio_service_types.h - Audio service type definitions (migrated from audio_service_module)
 */

#pragma once
#ifndef __AUDIO_SERVICE_TYPES_H__
#define __AUDIO_SERVICE_TYPES_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Audio service module configuration
     */
    typedef struct
    {
        int  sample_rate;            /*!< Audio sample rate (Hz) */
        int  channels;               /*!< Number of channels (1=mono, 2=stereo) */
        int  opus_frame_duration_ms; /*!< Opus frame duration (ms) */
        bool enable_aec;             /*!< Enable AEC (Acoustic Echo Cancellation) */
        bool enable_vad;             /*!< Enable VAD (Voice Activity Detection) */
        bool enable_wake_word;       /*!< Enable wake word detection */
        int  task_stack_size;        /*!< Task stack size */
        int  task_priority;          /*!< Task priority */
    } audio_service_module_config_t;

    /**
     * @brief Audio service module callbacks
     */
    typedef struct
    {
        void (*on_send_queue_available)(void); /*!< Called when send queue has space */
        void (*on_wake_word_detected)(const char* wake_word); /*!< Called when wake word detected */
        void (*on_vad_change)(bool speaking);                 /*!< Called when VAD state changes */
        void (*on_audio_testing_queue_full)(void); /*!< Called when testing queue is full */
        void (*on_event)(int event, void* event_data,
                         void* ctx); /*!< Called when xiaozhi event occurs */
    } audio_service_module_callbacks_t;

    /**
     * @brief Audio service module state
     */
    typedef enum
    {
        AUDIO_SERVICE_STATE_UNINIT = 0, /*!< Not initialized */
        AUDIO_SERVICE_STATE_INIT,       /*!< Initialized but not started */
        AUDIO_SERVICE_STATE_RUNNING,    /*!< Running */
        AUDIO_SERVICE_STATE_STOPPED,    /*!< Stopped */
    } audio_service_module_state_t;

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SERVICE_TYPES_H__ */
