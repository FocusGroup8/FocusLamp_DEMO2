#include "focuslamp_discovery.h"
#include "sdkconfig.h"

#include <string.h>

#if CONFIG_FOCUSLAMP_DISCOVERY_ENABLE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_manager.h"

#define KIT_MAX 32
#define BEACON_MAX 128
#define CHECK_MS 1000
#define HTTP_TIMEOUT_MS 2000
#define TX_DONE BIT0
#define RX_DONE BIT1
#define ALL_DONE (TX_DONE | RX_DONE)

static const char *TAG = "fl_discovery";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t s_done;
static bool s_initialized;
static bool s_running;
static bool s_stopping;
static bool s_identity_ready;
static bool s_local_has_ip;
static bool s_link_lost_hint;
static uint32_t s_ip;
static uint32_t s_netmask;
static uint32_t s_link_epoch;
static int64_t s_fast_started_ms = -1;
static int64_t s_next_tx_ms;
static int64_t s_next_summary_ms;
static bool s_first_tx_logged;
static int s_socket = -1;
static focuslamp_role_t s_self_role;
static char s_kit[KIT_MAX + 1];
static uint16_t s_http_port;
static uint32_t s_tx_count;
static uint32_t s_rx_count;

typedef struct {
    focuslamp_endpoint_t endpoint;
    uint32_t generation;
    int64_t last_attempt_ms;
} peer_record_t;

static peer_record_t s_peers[FOCUSLAMP_ROLE_MAX];

typedef struct {
    bool up;
    uint32_t ip;
    uint32_t netmask;
} link_snapshot_t;

typedef struct {
    focuslamp_role_t role;
    focuslamp_endpoint_t endpoint;
    uint32_t generation;
    uint32_t link_epoch;
    uint32_t local_ip;
    uint32_t local_netmask;
} verification_t;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *role_name(focuslamp_role_t role)
{
    return role == FOCUSLAMP_ROLE_HEAD ? "head" : "controller";
}

static bool valid_kit(const char *kit)
{
    if (!kit) {
        return false;
    }
    size_t len = strlen(kit);
    if (len == 0 || len > KIT_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)kit[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* wifi_manager_get_info() has no netmask and also performs a remote AP-info
 * RPC. Poll the manager's connected flag and the existing STA netif directly
 * so the one-second checkpoint never waits on that RPC. */
static link_snapshot_t read_link(void)
{
    link_snapshot_t result = {0};
    if (!wifi_manager_is_connected()) {
        return result;
    }
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
        info.ip.addr != 0 && info.netmask.addr != 0 && wifi_manager_is_connected()) {
        result.up = true;
        result.ip = info.ip.addr;
        result.netmask = info.netmask.addr;
    }
    return result;
}

static void invalidate_peers_locked(void)
{
    for (int i = 0; i < FOCUSLAMP_ROLE_MAX; ++i) {
        s_peers[i].endpoint.verified = false;
        s_peers[i].endpoint.stale = true;
        s_peers[i].generation++;
        s_peers[i].last_attempt_ms = 0;
    }
}

/* Runs in the TX/checkpoint task, independently of serial HTTP validation. */
static void check_link_and_ttl(void)
{
    link_snapshot_t link = read_link();
    int64_t now = now_ms();
    bool invalidated[FOCUSLAMP_ROLE_MAX] = {0};
    bool expired[FOCUSLAMP_ROLE_MAX] = {0};
    focuslamp_endpoint_t snapshots[FOCUSLAMP_ROLE_MAX];

    taskENTER_CRITICAL(&s_lock);
    bool changed = s_link_lost_hint || link.up != s_local_has_ip ||
                   link.ip != s_ip || link.netmask != s_netmask;
    s_link_lost_hint = false;
    if (changed) {
        for (int i = 0; i < FOCUSLAMP_ROLE_MAX; ++i) {
            invalidated[i] = s_peers[i].endpoint.host[0] != '\0' &&
                             !s_peers[i].endpoint.stale;
        }
        invalidate_peers_locked();
        s_link_epoch++;
        s_fast_started_ms = -1;
        s_first_tx_logged = false;
    }
    s_local_has_ip = link.up;
    s_ip = link.ip;
    s_netmask = link.netmask;
    if (s_running && !s_stopping && s_identity_ready && link.up && s_fast_started_ms < 0) {
        s_fast_started_ms = now;
        s_next_tx_ms = now;
    }
    for (int i = 0; i < FOCUSLAMP_ROLE_MAX; ++i) {
        focuslamp_endpoint_t *peer = &s_peers[i].endpoint;
        if (peer->host[0] && !peer->stale &&
            now - peer->last_seen_ms > CONFIG_FOCUSLAMP_DISCOVERY_PEER_TTL_MS) {
            peer->verified = false;
            peer->stale = true;
            s_peers[i].generation++;
            expired[i] = true;
        }
        snapshots[i] = *peer;
    }
    taskEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < FOCUSLAMP_ROLE_MAX; ++i) {
        if (invalidated[i]) {
            ESP_LOGW(TAG, "local link %s: peer %s invalidated",
                     link.up ? "changed" : "down", role_name(i));
        }
        if (expired[i]) {
            ESP_LOGW(TAG, "peer %s stale (last_seen %llds ago, keeping %s for diagnosis)",
                     role_name(i), (long long)((now - snapshots[i].last_seen_ms) / 1000),
                     snapshots[i].host);
        }
    }
}

esp_err_t focuslamp_discovery_get_peer(focuslamp_role_t role, focuslamp_endpoint_t *out)
{
    if (!out || (int)role < 0 || role >= FOCUSLAMP_ROLE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    link_snapshot_t live = read_link();
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_lock);
    *out = s_peers[role].endpoint;
    bool identity = s_identity_ready;
    bool local = s_initialized && s_running && !s_stopping && !s_link_lost_hint && s_local_has_ip &&
                 live.up && live.ip == s_ip && live.netmask == s_netmask;
    if (!local || (out->host[0] &&
        now - out->last_seen_ms > CONFIG_FOCUSLAMP_DISCOVERY_PEER_TTL_MS)) {
        out->verified = false;
        out->stale = true;
    }
    bool usable = identity && local && out->verified && !out->stale;
    taskEXIT_CRITICAL(&s_lock);
    if (!usable) {
        ESP_LOGI(TAG, "peer %s not ready (identity=%d local_ip=%d verified=%d stale=%d)",
                 role_name(role), identity, local, out->verified, out->stale);
    }
    return usable ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool should_stop(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool stop = s_stopping;
    taskEXIT_CRITICAL(&s_lock);
    return stop;
}

static void send_beacon(void)
{
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_lock);
    bool ready = s_running && !s_stopping && !s_link_lost_hint && s_identity_ready && s_local_has_ip &&
                 s_fast_started_ms >= 0 && now >= s_next_tx_ms;
    bool fast = s_fast_started_ms >= 0 &&
                now - s_fast_started_ms < CONFIG_FOCUSLAMP_DISCOVERY_FAST_WINDOW_MS;
    uint32_t ip = s_ip;
    uint32_t netmask = s_netmask;
    int fd = s_socket;
    if (ready) {
        s_next_tx_ms = now + (fast ? CONFIG_FOCUSLAMP_DISCOVERY_FAST_MS :
                                   CONFIG_FOCUSLAMP_DISCOVERY_SLOW_MS);
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!ready || !wifi_manager_is_connected()) {
        return;
    }

    char packet[BEACON_MAX + 1];
    int len = snprintf(packet, sizeof(packet), "FOCUSLAMP/1 role=%s kit=%s http=%u",
                       role_name(s_self_role), s_kit, (unsigned)s_http_port);
    if (len <= 0 || len > BEACON_MAX) {
        return;
    }
    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_FOCUSLAMP_DISCOVERY_UDP_PORT),
    };
    target.sin_addr.s_addr = htonl((ntohl(ip) & ntohl(netmask)) | ~ntohl(netmask));
    char directed[16];
    inet_ntop(AF_INET, &target.sin_addr, directed, sizeof(directed));
    uint32_t sent = 0;
    if (sendto(fd, packet, len, 0, (struct sockaddr *)&target, sizeof(target)) == len) {
        sent++;
    } else {
        ESP_LOGW(TAG, "directed beacon send failed: errno=%d", errno);
    }
    target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(fd, packet, len, 0, (struct sockaddr *)&target, sizeof(target)) == len) {
        sent++;
    } else {
        ESP_LOGW(TAG, "limited beacon send failed: errno=%d", errno);
    }
    taskENTER_CRITICAL(&s_lock);
    s_tx_count += sent;
    bool first = !s_first_tx_logged && sent != 0;
    if (first) {
        s_first_tx_logged = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (first) {
        ESP_LOGI(TAG, "beacon sent directed=%s limited=255.255.255.255 udp=%d phase=%s",
                 directed, CONFIG_FOCUSLAMP_DISCOVERY_UDP_PORT, fast ? "fast" : "steady");
    }
}

static void log_summary(void)
{
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_lock);
    bool due = now >= s_next_summary_ms;
    if (due) {
        s_next_summary_ms = now + 30000;
    }
    bool identity = s_identity_ready;
    bool local = s_local_has_ip;
    uint32_t ip = s_ip;
    uint32_t netmask = s_netmask;
    uint32_t tx = s_tx_count;
    uint32_t rx = s_rx_count;
    taskEXIT_CRITICAL(&s_lock);
    if (!due) {
        return;
    }
    focuslamp_role_t peer_role = s_self_role == FOCUSLAMP_ROLE_HEAD ?
                                FOCUSLAMP_ROLE_CONTROLLER : FOCUSLAMP_ROLE_HEAD;
    focuslamp_endpoint_t peer;
    esp_err_t result = focuslamp_discovery_get_peer(peer_role, &peer);
    struct in_addr broadcast = {
        .s_addr = htonl((ntohl(ip) & ntohl(netmask)) | ~ntohl(netmask)),
    };
    char directed[16];
    inet_ntop(AF_INET, &broadcast, directed, sizeof(directed));
    ESP_LOGI(TAG, "discovery summary: role=%s identity=%d local_ip=%d peer=%s %s:%u "
             "verified=%d stale=%d get_peer=%s tx=%" PRIu32 " rx=%" PRIu32 " directed=%s",
             role_name(s_self_role), identity, local, role_name(peer_role),
             peer.host[0] ? peer.host : "-", (unsigned)peer.port,
             peer.verified, peer.stale, esp_err_to_name(result), tx, rx, directed);
}

static void tx_task(void *arg)
{
    (void)arg;
    while (!should_stop()) {
        check_link_and_ttl();
        send_beacon();
        log_summary();
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
    }
    xEventGroupSetBits(s_done, TX_DONE);
    vTaskDelete(NULL);
}

typedef enum { PACKET_INVALID, PACKET_KIT_MISMATCH, PACKET_OK } packet_result_t;

static packet_result_t parse_packet(char *packet, int len, focuslamp_role_t *role,
                                   uint16_t *port, char *peer_kit)
{
    if (len <= 12 || len > BEACON_MAX || memchr(packet, '\0', len) ||
        strncmp(packet, "FOCUSLAMP/1 ", 12) != 0) {
        return PACKET_INVALID;
    }
    char *save;
    char *role_text = NULL;
    char *kit_text = NULL;
    char *port_text = NULL;
    for (char *token = strtok_r(packet + 12, " ", &save); token;
         token = strtok_r(NULL, " ", &save)) {
        if (strncmp(token, "role=", 5) == 0) {
            if (role_text) return PACKET_INVALID;
            role_text = token + 5;
        } else if (strncmp(token, "kit=", 4) == 0) {
            if (kit_text) return PACKET_INVALID;
            kit_text = token + 4;
        } else if (strncmp(token, "http=", 5) == 0) {
            if (port_text) return PACKET_INVALID;
            port_text = token + 5;
        }
        /* Unknown fields, including any self-reported IP, are never used. */
    }
    if (!role_text || !kit_text || !port_text || !valid_kit(kit_text) || !port_text[0]) {
        return PACKET_INVALID;
    }
    if (strcmp(role_text, "head") == 0) {
        *role = FOCUSLAMP_ROLE_HEAD;
    } else if (strcmp(role_text, "controller") == 0) {
        *role = FOCUSLAMP_ROLE_CONTROLLER;
    } else {
        return PACKET_INVALID;
    }
    if (*role == s_self_role) {
        return PACKET_INVALID;
    }
    unsigned value = 0;
    for (const char *p = port_text; *p; ++p) {
        if (*p < '0' || *p > '9') return PACKET_INVALID;
        value = value * 10 + (unsigned)(*p - '0');
        if (value > 65535) return PACKET_INVALID;
    }
    if (value == 0) return PACKET_INVALID;
    *port = (uint16_t)value;
    memcpy(peer_kit, kit_text, strlen(kit_text) + 1);
    return strcmp(kit_text, s_kit) == 0 ? PACKET_OK : PACKET_KIT_MISMATCH;
}

typedef struct {
    char data[512];
    size_t len;
    bool overflow;
} identity_response_t;

static esp_err_t collect_identity(esp_http_client_event_t *event)
{
    identity_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response) {
        return ESP_OK;
    }
    if (event->data_len < 0 || response->len + (size_t)event->data_len >= sizeof(response->data)) {
        response->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response->data + response->len, event->data, event->data_len);
    response->len += event->data_len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

static bool verify_identity(const verification_t *candidate)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u/api/device/info",
             candidate->endpoint.host, (unsigned)candidate->endpoint.port);
    identity_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = collect_identity,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "identity HTTP client allocation failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || response.overflow || response.len == 0 ||
        memchr(response.data, '\0', response.len)) {
        ESP_LOGW(TAG, "identity rejected: HTTP result=%s status=%d for %s:%u",
                 esp_err_to_name(err), status, candidate->endpoint.host,
                 (unsigned)candidate->endpoint.port);
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(response.data, NULL, true);
    cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
    cJSON *kit = cJSON_GetObjectItemCaseSensitive(root, "kit_id");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "http_port");
    bool valid = cJSON_IsObject(root) && cJSON_IsString(protocol) &&
                 strcmp(protocol->valuestring, "FOCUSLAMP/1") == 0 &&
                 cJSON_IsString(role) && strcmp(role->valuestring, role_name(candidate->role)) == 0 &&
                 cJSON_IsString(kit) && strcmp(kit->valuestring, s_kit) == 0 &&
                 cJSON_IsNumber(port) && port->valuedouble == candidate->endpoint.port;
    if (!valid) {
        ESP_LOGW(TAG, "identity rejected: protocol/role/kit_id/http_port mismatch for %s:%u",
                 candidate->endpoint.host, (unsigned)candidate->endpoint.port);
    }
    cJSON_Delete(root);
    return valid;
}

static void receive_candidate(char *packet, int len, const struct sockaddr_in *source)
{
    focuslamp_role_t role;
    uint16_t port;
    char kit[KIT_MAX + 1];
    packet_result_t result = parse_packet(packet, len, &role, &port, kit);
    if (result != PACKET_OK) {
        if (result == PACKET_KIT_MISMATCH) {
            ESP_LOGW(TAG, "beacon rejected: kit_id mismatch (local=%s peer=%s)", s_kit, kit);
        } else {
            ESP_LOGD(TAG, "beacon rejected: invalid packet (len=%d)", len);
        }
        return;
    }
    link_snapshot_t live = read_link();
    if (!live.up) {
        return;
    }
    focuslamp_endpoint_t incoming = {.port = port};
    if (!inet_ntop(AF_INET, &source->sin_addr, incoming.host, sizeof(incoming.host))) {
        return;
    }
    incoming.last_seen_ms = now_ms();
    verification_t candidate;
    taskENTER_CRITICAL(&s_lock);
    if (!s_running || s_stopping || s_link_lost_hint || !s_identity_ready || !s_local_has_ip ||
        s_ip != live.ip || s_netmask != live.netmask) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    peer_record_t *peer = &s_peers[role];
    bool changed = strcmp(peer->endpoint.host, incoming.host) != 0 ||
                   peer->endpoint.port != incoming.port;
    bool old = peer->endpoint.stale || (peer->endpoint.host[0] &&
               incoming.last_seen_ms - peer->endpoint.last_seen_ms >
               CONFIG_FOCUSLAMP_DISCOVERY_PEER_TTL_MS);
    bool fresh = changed || old;
    if (fresh) {
        peer->generation++;
        peer->last_attempt_ms = 0;
        peer->endpoint = incoming;
    } else {
        peer->endpoint.last_seen_ms = incoming.last_seen_ms;
        peer->endpoint.stale = false;
    }
    bool verify = !peer->endpoint.verified &&
                  (fresh || peer->last_attempt_ms == 0 ||
                   incoming.last_seen_ms - peer->last_attempt_ms >= CHECK_MS);
    if (verify) {
        peer->last_attempt_ms = incoming.last_seen_ms;
        candidate = (verification_t){role, peer->endpoint, peer->generation,
                                      s_link_epoch, s_ip, s_netmask};
    }
    taskEXIT_CRITICAL(&s_lock);
    if (fresh) {
        ESP_LOGI(TAG, "candidate %s %s:%u (unverified)", role_name(role),
                 incoming.host, (unsigned)port);
    }
    if (!verify) {
        return;
    }
    bool valid = verify_identity(&candidate);
    live = read_link();
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_lock);
    peer = &s_peers[role];
    bool current = s_running && !s_stopping && !s_link_lost_hint && s_identity_ready && s_local_has_ip &&
                   live.up && live.ip == candidate.local_ip &&
                   live.netmask == candidate.local_netmask &&
                   s_link_epoch == candidate.link_epoch &&
                   peer->generation == candidate.generation &&
                   strcmp(peer->endpoint.host, candidate.endpoint.host) == 0 &&
                   peer->endpoint.port == candidate.endpoint.port &&
                   now - peer->endpoint.last_seen_ms <= CONFIG_FOCUSLAMP_DISCOVERY_PEER_TTL_MS;
    if (current) {
        /* Timestamp completion too, so the second broadcast of a pair cannot
         * immediately start another request after a two-second failure. */
        peer->last_attempt_ms = now;
        peer->endpoint.verified = valid;
        peer->endpoint.stale = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (current && valid) {
        ESP_LOGI(TAG, "verified %s %s:%u kit=%s", role_name(role),
                 candidate.endpoint.host, (unsigned)candidate.endpoint.port, s_kit);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    char packet[256];
    while (!should_stop()) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = recvfrom(s_socket, packet, sizeof(packet) - 1, 0,
                           (struct sockaddr *)&source, &source_len);
        if (len < 0) {
            if (!should_stop() && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(TAG, "beacon receive failed: errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        taskENTER_CRITICAL(&s_lock);
        s_rx_count++;
        taskEXIT_CRITICAL(&s_lock);
        if (len == 0 || len > BEACON_MAX) {
            ESP_LOGD(TAG, "beacon rejected: invalid length=%d", len);
            continue;
        }
        packet[len] = '\0';
        if (!should_stop()) {
            receive_candidate(packet, len, &source);
        }
    }
    xEventGroupSetBits(s_done, RX_DONE);
    vTaskDelete(NULL);
}

esp_err_t focuslamp_discovery_init(const focuslamp_discovery_config_t *cfg)
{
    if (!cfg || (int)cfg->self_role < 0 || cfg->self_role >= FOCUSLAMP_ROLE_MAX ||
        !valid_kit(cfg->kit_id) || cfg->http_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return cfg->self_role == s_self_role && cfg->http_port == s_http_port &&
               strcmp(cfg->kit_id, s_kit) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_done = xEventGroupCreate();
    if (!s_done) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_done, ALL_DONE);
    taskENTER_CRITICAL(&s_lock);
    s_self_role = cfg->self_role;
    s_http_port = cfg->http_port;
    memcpy(s_kit, cfg->kit_id, strlen(cfg->kit_id) + 1);
    invalidate_peers_locked();
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t focuslamp_discovery_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool allowed = s_initialized && s_identity_ready && !s_running && !s_stopping;
    taskEXIT_CRITICAL(&s_lock);
    if (!allowed) {
        return ESP_ERR_INVALID_STATE;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "discovery socket allocation failed: errno=%d", errno);
        return ESP_FAIL;
    }
    int broadcast = 1;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_FOCUSLAMP_DISCOVERY_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "discovery socket setup failed: errno=%d", errno);
        close(fd);
        return ESP_FAIL;
    }
    int64_t started = now_ms();
    taskENTER_CRITICAL(&s_lock);
    s_socket = fd;
    s_running = true;
    s_fast_started_ms = -1;
    s_first_tx_logged = false;
    s_next_summary_ms = started + 30000;
    s_tx_count = 0;
    s_rx_count = 0;
    taskEXIT_CRITICAL(&s_lock);
    check_link_and_ttl();

    xEventGroupClearBits(s_done, TX_DONE);
    if (xTaskCreate(tx_task, "fl_disc_tx", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        xEventGroupSetBits(s_done, TX_DONE);
        focuslamp_discovery_stop();
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_done, RX_DONE);
    if (xTaskCreate(rx_task, "fl_disc_rx", 8192, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        xEventGroupSetBits(s_done, RX_DONE);
        focuslamp_discovery_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "discovery started role=%s kit=%s udp=%d", role_name(s_self_role),
             s_kit, CONFIG_FOCUSLAMP_DISCOVERY_UDP_PORT);
    return ESP_OK;
}

void focuslamp_discovery_stop(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool running = s_running;
    if (running) {
        s_stopping = true;
    }
    s_identity_ready = false;
    s_local_has_ip = false;
    s_fast_started_ms = -1;
    s_link_epoch++;
    invalidate_peers_locked();
    taskEXIT_CRITICAL(&s_lock);
    if (!running) {
        return;
    }
    /* RX owns its synchronous HTTP call. Both done bits mean that call has
     * been cleaned up as well; do not close the shared UDP socket earlier. */
    xEventGroupWaitBits(s_done, ALL_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    int fd = s_socket;
    s_socket = -1;
    taskEXIT_CRITICAL(&s_lock);
    if (fd >= 0) {
        close(fd);
    }
    taskENTER_CRITICAL(&s_lock);
    s_running = false;
    s_stopping = false;
    taskEXIT_CRITICAL(&s_lock);
}

void focuslamp_discovery_on_wifi_event(wifi_manager_event_t event, void *data)
{
    (void)data;
    if (event == WIFI_MANAGER_EVENT_DISCONNECTED || event == WIFI_MANAGER_EVENT_HOSTED_TIMEOUT) {
        taskENTER_CRITICAL(&s_lock);
        if (s_initialized) {
            s_link_lost_hint = true;
        }
        taskEXIT_CRITICAL(&s_lock);
    }
}

void focuslamp_discovery_set_identity_ready(bool ready)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_identity_ready != ready) {
        s_identity_ready = ready;
        s_fast_started_ms = -1;
        s_first_tx_logged = false;
        s_link_epoch++;
        invalidate_peers_locked();
    }
    taskEXIT_CRITICAL(&s_lock);
}

#else

esp_err_t focuslamp_discovery_init(const focuslamp_discovery_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t focuslamp_discovery_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void focuslamp_discovery_stop(void) {}
void focuslamp_discovery_set_identity_ready(bool ready) { (void)ready; }
void focuslamp_discovery_on_wifi_event(wifi_manager_event_t event, void *data)
{
    (void)event;
    (void)data;
}

esp_err_t focuslamp_discovery_get_peer(focuslamp_role_t role, focuslamp_endpoint_t *out)
{
    if (!out || (int)role < 0 || role >= FOCUSLAMP_ROLE_MAX) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->stale = true;
    return ESP_ERR_NOT_FOUND;
}

#endif
