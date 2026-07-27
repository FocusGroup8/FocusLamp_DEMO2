#include "ogg_opus_wrapper.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "esp_log.h"

static const char *TAG = "ogg_opus_wrapper";

extern "C" {

ogg_opus_decoder_handle_t ogg_opus_decoder_create(void)
{
    auto *decoder = new micro_opus::OggOpusDecoder();
    if (decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate OggOpusDecoder");
        return nullptr;
    }
    return static_cast<ogg_opus_decoder_handle_t>(decoder);
}

void ogg_opus_decoder_destroy(ogg_opus_decoder_handle_t handle)
{
    if (handle == nullptr) {
        return;
    }
    auto *decoder = static_cast<micro_opus::OggOpusDecoder*>(handle);
    delete decoder;
}

esp_err_t ogg_opus_decoder_decode(
    ogg_opus_decoder_handle_t handle,
    const uint8_t *input_data,
    size_t input_len,
    int16_t *output_pcm,
    size_t output_size,
    size_t *bytes_consumed,
    size_t *samples_decoded
)
{
    if (handle == nullptr || input_data == nullptr || output_pcm == nullptr) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    auto *decoder = static_cast<micro_opus::OggOpusDecoder*>(handle);

    ESP_LOGI(TAG, "Decoding: input_len=%zu, output_size=%zu, output_pcm=%p", 
             input_len, output_size, output_pcm);

    micro_opus::OggOpusResult result = decoder->decode(
        input_data, input_len,
        reinterpret_cast<uint8_t*>(output_pcm), output_size,
        *bytes_consumed, *samples_decoded
    );

    ESP_LOGI(TAG, "Decode result: %d, bytes_consumed=%zu, samples_decoded=%zu", 
             (int)result, *bytes_consumed, *samples_decoded);

    if (result == micro_opus::OGG_OPUS_OK && *samples_decoded > 0) {
        int16_t min_val = output_pcm[0];
        int16_t max_val = output_pcm[0];
        for (size_t i = 0; i < *samples_decoded; i++) {
            if (output_pcm[i] < min_val) min_val = output_pcm[i];
            if (output_pcm[i] > max_val) max_val = output_pcm[i];
        }
        ESP_LOGI(TAG, "Output range: min=%d, max=%d", min_val, max_val);
    }

    if (result == micro_opus::OGG_OPUS_OK) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Decode error: %d", (int)result);
        return ESP_FAIL;
    }
}

uint32_t ogg_opus_decoder_get_sample_rate(ogg_opus_decoder_handle_t handle)
{
    if (handle == nullptr) {
        return 0;
    }
    auto *decoder = static_cast<micro_opus::OggOpusDecoder*>(handle);
    return decoder->get_sample_rate();
}

uint8_t ogg_opus_decoder_get_channels(ogg_opus_decoder_handle_t handle)
{
    if (handle == nullptr) {
        return 0;
    }
    auto *decoder = static_cast<micro_opus::OggOpusDecoder*>(handle);
    return decoder->get_channels();
}

}
