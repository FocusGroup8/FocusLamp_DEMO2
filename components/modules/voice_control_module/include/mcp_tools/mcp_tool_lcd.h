#ifndef MCP_TOOL_LCD_H
#define MCP_TOOL_LCD_H

#include "esp_err.h"
#include "esp_mcp_tool.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 创建 LCD 控制相关的 MCP 工具 */
    esp_err_t mcp_tool_lcd_create(esp_mcp_tool_t** tools, int* count);

#ifdef __cplusplus
}
#endif

#endif // MCP_TOOL_LCD_H