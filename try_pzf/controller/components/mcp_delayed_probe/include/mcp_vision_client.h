#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "mcp_delayed_probe.h"

#define MCP_VISION_TOOL_NAME "self.vision.recognize"
/* RAM-only address, registered by the PC. HTTP IPv4 LAN origin, no path. */
esp_err_t mcp_vision_set_url(const char *url);
void mcp_vision_get_url(char *out, size_t size);
/* Returns owned text (free by caller); never performs a second POST. */
char *mcp_vision_answer(const char *question, mcp_probe_guard_t current, void *context, bool *failed);
