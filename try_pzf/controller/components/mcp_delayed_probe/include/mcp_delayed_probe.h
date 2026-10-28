#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct mcp_delayed_probe mcp_delayed_probe_t;
typedef bool (*mcp_probe_guard_t)(void *context);
/* send MUST recheck guard after obtaining the WebSocket send/state lock. */
typedef esp_err_t (*mcp_probe_send_t)(void *context, const char *wire,
                                    mcp_probe_guard_t guard, void *guard_context);
/* Background computation returns an owned JSON-RPC envelope, freed by worker. */
typedef char *(*mcp_probe_work_t)(const char *id_json, const char *session, const char *question,
                                mcp_probe_guard_t current, void *context);
#define MCP_PROBE_DUPLICATE 0x7301
#define MCP_PROBE_TOOL_NAME "self.test.delayed_result"

mcp_delayed_probe_t *mcp_delayed_probe_create(mcp_probe_send_t send, void *context);
void mcp_delayed_probe_resume(mcp_delayed_probe_t *probe);
/* Callback-safe: invalidate only, never wait or free transport objects. */
void mcp_delayed_probe_connection(mcp_delayed_probe_t *probe, bool online);
void mcp_delayed_probe_session_changed(mcp_delayed_probe_t *probe);
/* Only teardown/control tasks may drain. No forced deletion of a worker. */
void mcp_delayed_probe_drain(mcp_delayed_probe_t *probe);
void mcp_delayed_probe_destroy(mcp_delayed_probe_t *probe);
/* Copies wire, JSON-encoded request ID and session. delay_ms is ONLY 0/3000. */
esp_err_t mcp_delayed_probe_submit(mcp_delayed_probe_t *probe, const char *wire,
                                 const char *request_id, const char *session,
                                 uint32_t delay_ms);
esp_err_t mcp_delayed_probe_submit_work(mcp_delayed_probe_t *probe, const char *request_id,
                                      const char *session, const char *question, mcp_probe_work_t work);
