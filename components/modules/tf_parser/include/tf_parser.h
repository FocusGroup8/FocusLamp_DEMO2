/**
 * @file tf_parser.h
 * @brief TF frame parser interface
 *
 * This file provides the interface for the TF frame parser, which handles
 * parsing and validation of TF protocol frames from radar data.
 *
 * The parser provides:
 * - Frame parsing with state machine
 * - Header and data checksum validation
 * - Frame structure extraction
 *
 * TF Frame Structure:
 * - Start of Frame (SOF): 1 byte (0x01)
 * - Frame ID: 2 bytes
 * - Data Length: 2 bytes
 * - Message Type: 2 bytes
 * - Header Checksum: 1 byte
 * - Data: Variable length
 * - Data Checksum: 1 byte
 *
 * @author CottonLin
 * @date 2026-04-23
 * @version 1.0.0
 *
 * @see tf_parser_config.h for parser configuration
 * @see docs/guides/radar_guide.md for usage guide
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tf_parser_config.h"

#ifdef TF_PARSER_ENABLE

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @defgroup TFParser TF Frame Parser
     * @brief TF protocol frame parser for radar data
     * @{
     */

    /**
     * @brief TF parser result enumeration
     */
    typedef enum
    {
        TF_PARSER_OK = 0,                /**< Frame parsed successfully */
        TF_PARSER_INCOMPLETE,            /**< Frame is incomplete, need more data */
        TF_PARSER_INVALID_ARG,           /**< Invalid argument provided */
        TF_PARSER_INVALID_LENGTH,        /**< Invalid frame length */
        TF_PARSER_HEADER_CHECKSUM_ERROR, /**< Header checksum validation failed */
        TF_PARSER_DATA_CHECKSUM_ERROR,   /**< Data checksum validation failed */
        TF_PARSER_BUFFER_OVERFLOW,       /**< Parser buffer overflow */
    } tf_parser_result_t;

    /**
     * @brief TF frame structure
     *
     * This structure represents a parsed TF frame with all its components.
     */
    typedef struct
    {
        uint16_t frame_id;                         /**< Frame identifier */
        uint16_t data_length;                      /**< Data payload length */
        uint16_t message_type;                     /**< Message type identifier */
        uint8_t  header_checksum;                  /**< Header checksum value */
        uint8_t  data[TF_PARSER_MAX_PAYLOAD_SIZE]; /**< Data payload */
        uint8_t  data_checksum;                    /**< Data checksum value */
        size_t   total_length;                     /**< Total frame length in bytes */
    } tf_frame_t;

    /**
     * @brief TF parser context structure
     *
     * This structure maintains the parser state for incremental parsing.
     */
    typedef struct
    {
        uint8_t buffer[TF_PARSER_MAX_FRAME_SIZE]; /**< Internal frame buffer */
        size_t  position;                         /**< Current buffer position */
        size_t  expected_frame_length;            /**< Expected frame length */
    } tf_parser_t;

    /**
     * @brief Initialize parser state
     *
     * This function initializes the parser context to its default state.
     *
     * @param[out] parser Parser context to initialize
     */
    void tf_parser_init(tf_parser_t* parser);

    /**
     * @brief Reset parser state and drop any buffered bytes
     *
     * This function resets the parser to its initial state, discarding
     * any partially parsed data.
     *
     * @param[in,out] parser Parser context to reset
     */
    void tf_parser_reset(tf_parser_t* parser);

    /**
     * @brief Feed one byte into the parser state machine
     *
     * This function processes a single byte through the parser state machine.
     * When a complete frame is parsed, it returns TF_PARSER_OK and fills
     * the frame structure.
     *
     * @param[in,out] parser Parser context
     * @param[in] byte New incoming byte
     * @param[out] frame Parsed frame when a full frame is completed
     *
     * @return tf_parser_result_t
     *      - TF_PARSER_OK: Frame parsed successfully
     *      - TF_PARSER_INCOMPLETE: Need more data
     *      - Other error codes on failure
     */
    tf_parser_result_t tf_parser_input(tf_parser_t* parser, uint8_t byte, tf_frame_t* frame);

    /**
     * @brief Parse and validate a full TF frame buffer
     *
     * This function parses a complete frame from a raw buffer and validates
     * both header and data checksums.
     *
     * @param[in] raw_frame Raw frame bytes
     * @param[in] raw_frame_length Raw frame size in bytes
     * @param[out] frame Parsed frame output
     *
     * @return tf_parser_result_t
     *      - TF_PARSER_OK: Frame parsed and validated successfully
     *      - TF_PARSER_INVALID_ARG: NULL pointer or zero length
     *      - TF_PARSER_INVALID_LENGTH: Frame length mismatch
     *      - TF_PARSER_HEADER_CHECKSUM_ERROR: Header checksum failed
     *      - TF_PARSER_DATA_CHECKSUM_ERROR: Data checksum failed
     */
    tf_parser_result_t tf_parser_parse_frame(const uint8_t* raw_frame, size_t raw_frame_length,
                                             tf_frame_t* frame);

    /**
     * @brief Validate only the header checksum of a raw TF frame
     *
     * This function validates the header checksum without parsing the full frame.
     *
     * @param[in] raw_frame Raw frame bytes
     * @param[in] raw_frame_length Raw frame size in bytes
     *
     * @return true Header checksum is valid
     * @return false Header checksum is invalid or arguments are invalid
     */
    bool tf_parser_validate_header_checksum(const uint8_t* raw_frame, size_t raw_frame_length);

    /**
     * @brief Validate only the data checksum of a raw TF frame
     *
     * This function validates the data checksum without parsing the full frame.
     *
     * @param[in] raw_frame Raw frame bytes
     * @param[in] raw_frame_length Raw frame size in bytes
     *
     * @return true Data checksum is valid
     * @return false Data checksum is invalid or arguments are invalid
     */
    bool tf_parser_validate_data_checksum(const uint8_t* raw_frame, size_t raw_frame_length);

    /** @} */

#ifdef __cplusplus
}
#endif

#else

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        TF_PARSER_OK = 0,
        TF_PARSER_INCOMPLETE,
        TF_PARSER_INVALID_ARG,
        TF_PARSER_INVALID_LENGTH,
        TF_PARSER_HEADER_CHECKSUM_ERROR,
        TF_PARSER_DATA_CHECKSUM_ERROR,
        TF_PARSER_BUFFER_OVERFLOW,
    } tf_parser_result_t;

    typedef struct
    {
        uint16_t frame_id;
        uint16_t data_length;
        uint16_t message_type;
        uint8_t  header_checksum;
        uint8_t  data[256];
        uint8_t  data_checksum;
        size_t   total_length;
    } tf_frame_t;

    typedef struct
    {
        uint8_t buffer[520];
        size_t  position;
        size_t  expected_frame_length;
    } tf_parser_t;

    static inline void tf_parser_init(tf_parser_t* parser)
    {
        (void)parser;
    }
    static inline void tf_parser_reset(tf_parser_t* parser)
    {
        (void)parser;
    }
    static inline tf_parser_result_t tf_parser_input(tf_parser_t* parser, uint8_t byte,
                                                     tf_frame_t* frame)
    {
        (void)parser;
        (void)byte;
        (void)frame;
        return TF_PARSER_INVALID_ARG;
    }
    static inline tf_parser_result_t
    tf_parser_parse_frame(const uint8_t* raw_frame, size_t raw_frame_length, tf_frame_t* frame)
    {
        (void)raw_frame;
        (void)raw_frame_length;
        (void)frame;
        return TF_PARSER_INVALID_ARG;
    }
    static inline bool tf_parser_validate_header_checksum(const uint8_t* raw_frame,
                                                          size_t         raw_frame_length)
    {
        (void)raw_frame;
        (void)raw_frame_length;
        return false;
    }
    static inline bool tf_parser_validate_data_checksum(const uint8_t* raw_frame,
                                                        size_t         raw_frame_length)
    {
        (void)raw_frame;
        (void)raw_frame_length;
        return false;
    }

#ifdef __cplusplus
}
#endif

#endif
