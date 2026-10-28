idf_component_register(SRCS "mcp_delayed_probe.c" "mcp_probe_tool.c" "mcp_vision_client.c"
    INCLUDE_DIRS "include" REQUIRES freertos esp_timer mcp-c-sdk esp_http_client json)
