#ifndef OGG_OPUS_WRAPPER_H
#define OGG_OPUS_WRAPPER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void* ogg_opus_decoder_handle_t;

    ogg_opus_decoder_handle_t ogg_opus_decoder_create(void);

    void ogg_opus_decoder_destroy(ogg_opus_decoder_handle_t handle);

    esp_err_t ogg_opus_decoder_decode(ogg_opus_decoder_handle_t handle, const uint8_t* input_data,
                                      size_t input_len, int16_t* output_pcm, size_t output_size,
                                      size_t* bytes_consumed, size_t* samples_decoded);

    uint32_t ogg_opus_decoder_get_sample_rate(ogg_opus_decoder_handle_t handle);

    uint8_t ogg_opus_decoder_get_channels(ogg_opus_decoder_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
