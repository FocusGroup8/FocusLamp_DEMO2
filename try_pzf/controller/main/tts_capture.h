#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Passive cloud-TTS tap. Disabled until an explicit LAN HTTP arm request.
 * Callbacks only protect small metadata in short critical sections; copying
 * happens outside. They never allocate, take an RTOS semaphore, perform I/O,
 * or change chat state. */
esp_err_t tts_capture_http_init(void);
void tts_capture_start(void);
void tts_capture_packet(const uint8_t *data, size_t len);
void tts_capture_sentence(const char *text);
void tts_capture_stop(void);
void tts_capture_abort(void);
