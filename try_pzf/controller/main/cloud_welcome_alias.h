#pragma once
#include <stdbool.h>

/* Legacy head commands remain cloud welcome triggers, never local playback. */
bool cloud_welcome_matches(const char *text);
