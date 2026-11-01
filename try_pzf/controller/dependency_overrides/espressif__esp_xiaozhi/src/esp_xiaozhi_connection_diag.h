#pragma once

#include <string.h>
#include "esp_log.h"

/* Never log the raw URL: userinfo, query and fragment may contain credentials. */
static inline void xiaozhi_diag_endpoint(const char *tag, const char *label, const char *url)
{
    const char *authority = url ? strstr(url, "://") : NULL;
    if (!authority) {
        ESP_LOGI(tag, "[XZ_LINK] %s endpoint=unavailable", label);
        return;
    }
    authority += 3;
    const char *end = authority + strcspn(authority, "/?#");
    const char *host = authority;
    for (const char *p = authority; p < end; ++p) {
        if (*p == '@') host = p + 1;
    }
    const char *path = *end == '/' ? end : "/";
    size_t host_len = (size_t)(end - host);
    size_t path_len = strcspn(path, "?#");
    if (!host_len || host_len > 255 || path_len > 255) {
        ESP_LOGI(tag, "[XZ_LINK] %s endpoint=invalid_or_too_long", label);
        return;
    }
    for (size_t i = 0; i < host_len + path_len; ++i) {
        unsigned char c = (unsigned char)(i < host_len ? host[i] : path[i - host_len]);
        if (c < 33 || c > 126) {
            ESP_LOGI(tag, "[XZ_LINK] %s endpoint=invalid_characters", label);
            return;
        }
    }
    ESP_LOGI(tag, "[XZ_LINK] %s host=%.*s path=%.*s", label,
             (int)host_len, host, (int)path_len, path);
}
