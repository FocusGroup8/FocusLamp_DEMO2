/*
 * app_mcp_handler.h - Remote MCP (JSON-RPC 2.0) tool handler
 *
 * Provides a JSON-RPC 2.0 based MCP (Model Context Protocol) endpoint
 * for remote control of the FocusLamp over WebSocket.
 *
 * Architecture:
 *   Remote client (AI model / browser / XiaoZhi)
 *     → WebSocket /mcp endpoint (websocket_manager)
 *     → ws_data_handler → app_mcp_handler_handle_data()
 *     → JSON-RPC 2.0 parser → tool dispatch → hardware control API
 *     → Response sent back via ws_manager_server_send_text()
 *
 * Supported methods:
 *   - initialize        - MCP initialization handshake
 *   - tools.list        - List available tools (LED, Display, LCD, System, Radar, Light, Servo)
 *   - tools.call        - Execute tool with arguments
 *   - ping              - Health check
 *
 * Protocol: JSON-RPC 2.0 over WebSocket text frames (/mcp endpoint)
 */

#pragma once
#ifndef APP_MCP_HANDLER_H
#define APP_MCP_HANDLER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MCP handler and register WebSocket DATA callback
 *
 * Must be called after ws_manager_init() + ws_manager_server_start() and
 * all hardware modules (led_service, lcd_module, lcd_driver) are initialized.
 * Registers a WS_MANAGER_EVENT_DATA handler that routes /mcp frames here.
 *
 * @return ESP_OK on success
 */
esp_err_t app_mcp_handler_init(void);

/**
 * @brief Handle incoming JSON-RPC 2.0 message from WebSocket /mcp client
 *
 * Parses the request, dispatches to the appropriate tool callback,
 * and sends the JSON-RPC response back via ws_manager_server_send_text().
 *
 * @param data       Incoming payload (JSON string, not null-terminated)
 * @param data_len   Payload length in bytes
 * @param client_fd  WebSocket client fd to send the response to
 * @return ESP_OK on success
 */
esp_err_t app_mcp_handler_handle_data(const char *data, int data_len, int client_fd);

#ifdef __cplusplus
}
#endif

#endif /* APP_MCP_HANDLER_H */
