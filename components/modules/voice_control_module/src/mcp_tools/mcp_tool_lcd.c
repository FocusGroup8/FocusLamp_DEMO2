#include "voice_control_module_config.h"

#if (VOICE_CONTROL_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_log.h"
#include "esp_mcp_tool.h"

#include "lcd_module.h"
#include "mcp_tool_lcd.h"

static const char* TAG = "mcp_tool_lcd";

static const struct
{
    const char*      name;
    lcd_expression_t expr;
} s_expression_map[] = {
    {"normal", LCD_EXPRESSION_NORMAL},
    {"happy", LCD_EXPRESSION_HAPPY},
    {"sad", LCD_EXPRESSION_SAD},
    {"angry", LCD_EXPRESSION_ANGRY},
    {"surprised", LCD_EXPRESSION_SURPRISED},
    {"sleepy", LCD_EXPRESSION_SLEEPY},
};

// 新版回调函数签名：返回 esp_mcp_value_t，参数为 property_list
static esp_mcp_value_t lcd_switch_page_callback(const esp_mcp_property_list_t* properties)
{
    ESP_LOGI(TAG, "Executing lcd.switch_page");
    esp_err_t ret = lcd_module_next_page();
    return esp_mcp_value_create_string(ret == ESP_OK ? "页面已切换" : "切换页面失败");
}

static esp_mcp_value_t lcd_set_expression_callback(const esp_mcp_property_list_t* properties)
{
    // 从属性列表中获取字符串参数
    const char* expr_name = esp_mcp_property_list_get_property_string(properties, "expression");
    if (expr_name == NULL)
    {
        return esp_mcp_value_create_string("参数错误：缺少 expression");
    }

    for (int i = 0; i < sizeof(s_expression_map) / sizeof(s_expression_map[0]); i++)
    {
        if (strcmp(expr_name, s_expression_map[i].name) == 0)
        {
            ESP_LOGI(TAG, "Setting expression: %s", expr_name);
            lcd_module_set_expression(s_expression_map[i].expr);
            return esp_mcp_value_create_string("表情已设置");
        }
    }
    return esp_mcp_value_create_string("未知表情类型");
}

static esp_mcp_value_t lcd_enable_blink_callback(const esp_mcp_property_list_t* properties)
{
    ESP_LOGI(TAG, "Executing lcd.enable_blink");
    lcd_module_set_auto_blink(true);
    return esp_mcp_value_create_string("已开启自动眨眼");
}

static esp_mcp_value_t lcd_disable_blink_callback(const esp_mcp_property_list_t* properties)
{
    ESP_LOGI(TAG, "Executing lcd.disable_blink");
    lcd_module_set_auto_blink(false);
    return esp_mcp_value_create_string("已关闭自动眨眼");
}

esp_err_t mcp_tool_lcd_create(esp_mcp_tool_t** tools, int* count)
{
    *count   = 4;
    // 使用新版创建接口
    tools[0] = esp_mcp_tool_create("lcd.switch_page", "切换显示页面", lcd_switch_page_callback);
    tools[1] =
        esp_mcp_tool_create("lcd.set_expression", "设置屏幕表情", lcd_set_expression_callback);
    tools[2] = esp_mcp_tool_create("lcd.enable_blink", "开启自动眨眼", lcd_enable_blink_callback);
    tools[3] = esp_mcp_tool_create("lcd.disable_blink", "关闭自动眨眼", lcd_disable_blink_callback);

    // 添加参数定义
    esp_mcp_property_t* prop = esp_mcp_property_create("expression", ESP_MCP_PROPERTY_TYPE_STRING);
    if (prop)
    {
        esp_mcp_tool_add_property(tools[1], prop);
    }

    return ESP_OK;
}

#endif // VOICE_CONTROL_MODULE_ENABLE