#include "tf_parser_config.h"

#ifdef TF_PARSER_ENABLE

#include <string.h>

#include "esp_log.h"

#include "tf_parser.h"

#if (RADAR_DEBUG_TF_PARSER_CHECKSUM == 1 || RADAR_DEBUG_TF_PARSER_RAW_DATA == 1 || \
     RADAR_DEBUG_TF_PARSER_FRAME == 1 || RADAR_DEBUG_TF_PARSER_STATE == 1)
static const char* TAG = "tf_parser";
#endif

#define RADAR_TF_SOF 0x01
#define RADAR_TF_HEADER_SIZE 8

static uint8_t tf_parser_calculate_checksum(const uint8_t* data, size_t length)
{
    uint8_t checksum = 0;

    for (size_t i = 0; i < length; ++i)
    {
        checksum ^= data[i];
    }

    uint8_t result = (uint8_t)(~checksum);

#if (RADAR_DEBUG_TF_PARSER_CHECKSUM == 1)
    ESP_LOGI(TAG, "Checksum calc: len=%u, XOR=0x%02X, result=0x%02X", (unsigned)length, checksum,
             result);
#endif

    return result;
}

static bool tf_parser_get_expected_length(const uint8_t* raw_frame, size_t raw_frame_length,
                                          size_t* expected_length)
{
    if (raw_frame == NULL || expected_length == NULL || raw_frame_length < RADAR_TF_HEADER_SIZE)
    {
        return false;
    }

    const uint16_t data_length = (uint16_t)((raw_frame[3] << 8) | raw_frame[4]);
    *expected_length           = RADAR_TF_HEADER_SIZE + data_length + 1;

    return (*expected_length <= TF_PARSER_MAX_FRAME_SIZE);
}

void tf_parser_init(tf_parser_t* parser)
{
    if (parser == NULL)
    {
        return;
    }

    tf_parser_reset(parser);
}

void tf_parser_reset(tf_parser_t* parser)
{
    if (parser == NULL)
    {
        return;
    }

    memset(parser->buffer, 0, sizeof(parser->buffer));
    parser->position              = 0;
    parser->expected_frame_length = 0;
}

bool tf_parser_validate_header_checksum(const uint8_t* raw_frame, size_t raw_frame_length)
{
    if (raw_frame == NULL || raw_frame_length < RADAR_TF_HEADER_SIZE)
    {
        return false;
    }

    uint8_t calculated = tf_parser_calculate_checksum(raw_frame, RADAR_TF_HEADER_SIZE - 1);
    uint8_t received   = raw_frame[7];
    bool    valid      = (calculated == received);

#if (RADAR_DEBUG_TF_PARSER_CHECKSUM == 1)
    ESP_LOGI(TAG, "Header checksum: calc=0x%02X, recv=0x%02X, valid=%d", calculated, received,
             valid);
#endif

    return valid;
}

bool tf_parser_validate_data_checksum(const uint8_t* raw_frame, size_t raw_frame_length)
{
    size_t expected_length = 0;
    if (!tf_parser_get_expected_length(raw_frame, raw_frame_length, &expected_length))
    {
        return false;
    }

    if (raw_frame_length < expected_length)
    {
        return false;
    }

    const uint16_t data_length = (uint16_t)((raw_frame[3] << 8) | raw_frame[4]);
    uint8_t        calculated =
        tf_parser_calculate_checksum(&raw_frame[RADAR_TF_HEADER_SIZE], data_length);
    uint8_t received = raw_frame[expected_length - 1];
    bool    valid    = (calculated == received);

#if (RADAR_DEBUG_TF_PARSER_CHECKSUM == 1)
    ESP_LOGI(TAG, "Data checksum: calc=0x%02X, recv=0x%02X, valid=%d", calculated, received, valid);
#endif

    return valid;
}

tf_parser_result_t tf_parser_parse_frame(const uint8_t* raw_frame, size_t raw_frame_length,
                                         tf_frame_t* frame)
{
    size_t expected_length = 0;

    if (raw_frame == NULL || frame == NULL)
    {
        return TF_PARSER_INVALID_ARG;
    }

#if (RADAR_DEBUG_TF_PARSER_RAW_DATA == 1)
    ESP_LOGI(TAG, "Raw frame (%u bytes):", (unsigned)raw_frame_length);
    for (size_t i = 0; i < raw_frame_length && i < 32; ++i)
    {
        printf("%02X ", raw_frame[i]);
        if ((i + 1) % 16 == 0)
        {
            printf("\n");
        }
    }
    printf("\n");
#endif

    if (!tf_parser_get_expected_length(raw_frame, raw_frame_length, &expected_length))
    {
        return TF_PARSER_INVALID_LENGTH;
    }

    if (raw_frame_length < expected_length)
    {
        return TF_PARSER_INCOMPLETE;
    }

    if (raw_frame[0] != RADAR_TF_SOF)
    {
        return TF_PARSER_INVALID_LENGTH;
    }

    if (!tf_parser_validate_header_checksum(raw_frame, raw_frame_length))
    {
        return TF_PARSER_HEADER_CHECKSUM_ERROR;
    }

    if (!tf_parser_validate_data_checksum(raw_frame, raw_frame_length))
    {
        return TF_PARSER_DATA_CHECKSUM_ERROR;
    }

    memset(frame, 0, sizeof(*frame));
    frame->frame_id        = (uint16_t)((raw_frame[1] << 8) | raw_frame[2]);
    frame->data_length     = (uint16_t)((raw_frame[3] << 8) | raw_frame[4]);
    frame->message_type    = (uint16_t)((raw_frame[5] << 8) | raw_frame[6]);
    frame->header_checksum = raw_frame[7];
    frame->data_checksum   = raw_frame[expected_length - 1];
    frame->total_length    = expected_length;

    if (frame->data_length > 0)
    {
        memcpy(frame->data, &raw_frame[RADAR_TF_HEADER_SIZE], frame->data_length);
    }

#if (RADAR_DEBUG_TF_PARSER_FRAME == 1)
    ESP_LOGI(TAG, "Frame parsed: ID=0x%04X, Type=0x%04X, Len=%u, Total=%u", frame->frame_id,
             frame->message_type, frame->data_length, (unsigned)frame->total_length);
    if (frame->data_length > 0 && frame->data_length <= 16)
    {
        printf("Data: ");
        for (size_t i = 0; i < frame->data_length; ++i)
        {
            printf("%02X ", frame->data[i]);
        }
        printf("\n");
    }
#endif

    return TF_PARSER_OK;
}

tf_parser_result_t tf_parser_input(tf_parser_t* parser, uint8_t byte, tf_frame_t* frame)
{
    if (parser == NULL || frame == NULL)
    {
        return TF_PARSER_INVALID_ARG;
    }

    if (parser->position == 0)
    {
        if (byte != RADAR_TF_SOF)
        {
#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
            ESP_LOGD(TAG, "Ignored byte 0x%02X (not SOF)", byte);
#endif
            return TF_PARSER_INCOMPLETE;
        }
#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
        ESP_LOGI(TAG, "SOF detected, starting frame");
#endif
    }

    if (parser->position >= sizeof(parser->buffer))
    {
#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
        ESP_LOGW(TAG, "Buffer overflow at position %u", (unsigned)parser->position);
#endif
        tf_parser_reset(parser);
        return TF_PARSER_BUFFER_OVERFLOW;
    }

    parser->buffer[parser->position++] = byte;

#if (RADAR_DEBUG_TF_PARSER_RAW_DATA == 1)
    ESP_LOGI(TAG, "Byte %u: 0x%02X", (unsigned)parser->position, byte);
#endif

    if (parser->position == 7)
    {
        const uint16_t data_length    = (uint16_t)((parser->buffer[3] << 8) | parser->buffer[4]);
        parser->expected_frame_length = RADAR_TF_HEADER_SIZE + data_length + 1;

#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
        ESP_LOGI(TAG, "Header complete: data_len=%u, expected_frame_len=%u", data_length,
                 (unsigned)parser->expected_frame_length);
#endif

        if (parser->expected_frame_length > sizeof(parser->buffer))
        {
#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
            ESP_LOGW(TAG, "Invalid frame length %u > buffer size %u",
                     (unsigned)parser->expected_frame_length, (unsigned)sizeof(parser->buffer));
#endif
            tf_parser_reset(parser);
            return TF_PARSER_INVALID_LENGTH;
        }
    }

    if (parser->expected_frame_length == 0 || parser->position < parser->expected_frame_length)
    {
        return TF_PARSER_INCOMPLETE;
    }

#if (RADAR_DEBUG_TF_PARSER_STATE == 1)
    ESP_LOGI(TAG, "Frame complete, parsing...");
#endif

    tf_parser_result_t result = tf_parser_parse_frame(parser->buffer, parser->position, frame);
    tf_parser_reset(parser);
    return result;
}

#endif
