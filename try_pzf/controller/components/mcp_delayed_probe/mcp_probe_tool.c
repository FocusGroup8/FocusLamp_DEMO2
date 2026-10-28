#include "mcp_probe_tool.h"
#include "mcp_delayed_probe.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"
#include "mcp_vision_client.h"

static esp_mcp_value_t diagnostic_fallback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    return esp_mcp_value_create_string("此诊断仅支持通过小智 WebSocket 对话调用；未执行延迟测试。");
}
static esp_err_t register_vision(esp_mcp_t *engine)
{
    esp_mcp_tool_t *tool=esp_mcp_tool_create(MCP_VISION_TOOL_NAME,
        "用户明确要求看摄像头、看看手里拿的物体、读画面文字或看图回答时使用。"
        "取台灯摄像头当前新鲜截图并发送到豆包识图，question填写用户具体问题。"
        "结果只对应本次截图。收到answer后在当前对话用原音色回答；失败请如实说明，不要猜测画面。",
        diagnostic_fallback);
    if (!tool) return ESP_ERR_NO_MEM;
    esp_mcp_property_t *question=esp_mcp_property_create_with_string("question","请描述当前摄像头画面中的物体。");
    if (!question) { esp_mcp_tool_destroy(tool);return ESP_ERR_NO_MEM; }
    esp_err_t ret=esp_mcp_tool_add_property(tool,question);
    if (ret!=ESP_OK) { esp_mcp_property_destroy(question);esp_mcp_tool_destroy(tool);return ret; }
    ret=esp_mcp_tool_set_task_support(tool,"forbidden");
    if (ret==ESP_OK) ret=esp_mcp_add_tool(engine,tool);
    if (ret!=ESP_OK) esp_mcp_tool_destroy(tool);
    return ret;
}

esp_err_t mcp_probe_register_tool(esp_mcp_t *engine)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(MCP_PROBE_TOOL_NAME,
        "仅在用户要求通信延迟测试时调用。delay_ms只允许0或3000；text是要返回的校验文字。"
        "收到结果后请用当前音色复述校验文字；这不是识图或播报注入工具。", diagnostic_fallback);
    if (!tool) return ESP_ERR_NO_MEM;
    esp_mcp_property_t *delay = esp_mcp_property_create_with_int("delay_ms", 3000);
    esp_mcp_property_t *text = esp_mcp_property_create_with_string("text", "延迟验证完成，校验码7301。");
    if (!delay || !text) {
        if (delay) esp_mcp_property_destroy(delay);
        if (text) esp_mcp_property_destroy(text);
        esp_mcp_tool_destroy(tool);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret=esp_mcp_tool_add_property(tool, delay);
    if (ret!=ESP_OK) {
        esp_mcp_property_destroy(delay); esp_mcp_property_destroy(text);
        esp_mcp_tool_destroy(tool); return ret;
    }
    ret=esp_mcp_tool_add_property(tool, text);
    if (ret!=ESP_OK) {
        esp_mcp_property_destroy(text); esp_mcp_tool_destroy(tool); return ret;
    }
    ret=esp_mcp_tool_set_task_support(tool, "forbidden");
    if (ret==ESP_OK) ret=esp_mcp_add_tool(engine, tool);
    if (ret!=ESP_OK) esp_mcp_tool_destroy(tool);
    return ret==ESP_OK ? register_vision(engine) : ret;
}
