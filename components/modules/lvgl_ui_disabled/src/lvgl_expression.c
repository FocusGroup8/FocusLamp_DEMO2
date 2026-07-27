#include "lvgl_expression.h"

#include "esp_log.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_EXPRESSION == 1)

#include "lcd_expression.h"

static const char* TAG = "LVGL_EXPR";

static bool                 s_initialized  = false;
static lvgl_ui_expression_t s_current_expr = LVGL_UI_EXPR_NORMAL;

static lcd_expression_t lvgl_expr_to_lcd(lvgl_ui_expression_t expr)
{
    switch (expr)
    {
    case LVGL_UI_EXPR_NORMAL:
        return LCD_EXPRESSION_NORMAL;
    case LVGL_UI_EXPR_HAPPY:
        return LCD_EXPRESSION_HAPPY;
    case LVGL_UI_EXPR_SAD:
        return LCD_EXPRESSION_SAD;
    case LVGL_UI_EXPR_ANGRY:
        return LCD_EXPRESSION_ANGRY;
    case LVGL_UI_EXPR_SURPRISED:
        return LCD_EXPRESSION_SURPRISED;
    case LVGL_UI_EXPR_SLEEPY:
        return LCD_EXPRESSION_SLEEPY;
    default:
        return LCD_EXPRESSION_NORMAL;
    }
}

esp_err_t lvgl_expression_init(lv_obj_t* parent)
{
    (void)parent;

    if (s_initialized)
    {
        ESP_LOGW(TAG, "Expression page already initialized");
        return ESP_OK;
    }

    esp_err_t ret = lcd_expression_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LCD expression: %s", esp_err_to_name(ret));
        return ret;
    }

    lcd_expression_set(lvgl_expr_to_lcd(s_current_expr));

    s_initialized = true;
    ESP_LOGI(TAG, "Expression page initialized (using LCD expression module)");
    return ESP_OK;
}

void lvgl_expression_deinit(void)
{
    if (!s_initialized)
    {
        return;
    }

    lcd_expression_deinit();
    s_initialized = false;
    ESP_LOGI(TAG, "Expression page deinitialized");
}

void lvgl_expression_update(void)
{
    if (!s_initialized)
    {
        return;
    }

    lcd_expression_update();
}

esp_err_t lvgl_expression_set(lvgl_ui_expression_t expr)
{
    if (expr < 0 || expr >= LVGL_UI_EXPR_COUNT)
    {
        ESP_LOGW(TAG, "Invalid expression: %d", expr);
        return ESP_ERR_INVALID_ARG;
    }

    s_current_expr = expr;

    if (s_initialized)
    {
        return lcd_expression_set(lvgl_expr_to_lcd(expr));
    }

    return ESP_OK;
}

lvgl_ui_expression_t lvgl_expression_get(void)
{
    return s_current_expr;
}

void lvgl_expression_set_auto_blink(bool enable)
{
    if (s_initialized)
    {
        lcd_expression_set_auto_blink(enable);
    }
}

bool lvgl_expression_is_auto_blink(void)
{
    if (s_initialized)
    {
        return lcd_expression_is_auto_blink();
    }
    return false;
}

#endif
