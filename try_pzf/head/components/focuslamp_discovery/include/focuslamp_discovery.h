#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifi_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOCUSLAMP_ROLE_CONTROLLER = 0,
    FOCUSLAMP_ROLE_HEAD,
    FOCUSLAMP_ROLE_MAX
} focuslamp_role_t;

typedef struct {
    char host[16];
    uint16_t port;
    int64_t last_seen_ms;
    bool verified;
    bool stale;
} focuslamp_endpoint_t;

typedef struct {
    focuslamp_role_t self_role;
    const char *kit_id;
    uint16_t http_port;
} focuslamp_discovery_config_t;

esp_err_t focuslamp_discovery_init(const focuslamp_discovery_config_t *cfg);
esp_err_t focuslamp_discovery_start(void);
void focuslamp_discovery_stop(void);

/* ESP_OK is the only permission to use out as a business endpoint.
 * ESP_ERR_NOT_FOUND may still return the old address for diagnosis. */
esp_err_t focuslamp_discovery_get_peer(focuslamp_role_t role,
                                     focuslamp_endpoint_t *out);

/* Auxiliary notification only: data may be NULL; no WiFi callbacks are registered. */
void focuslamp_discovery_on_wifi_event(wifi_manager_event_t event, void *data);
void focuslamp_discovery_set_identity_ready(bool ready);

#ifdef __cplusplus
}
#endif
