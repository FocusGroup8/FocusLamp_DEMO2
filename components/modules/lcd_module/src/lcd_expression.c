#include "lcd_expression.h"

#include "esp_log.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_config.h"
#include "lcd_driver.h"

#if (RADAR_ENABLE_LCD_DISPLAY == 1)

static const char* TAG = "LCD_EXPRESSION";

#define LCD_EYE_BASE_WIDTH 30
#define LCD_EYE_BASE_HEIGHT 30
#define LCD_EYE_SPACING 80
#define LCD_EYE_CENTER_X (LCD_WIDTH / 2)
#define LCD_EYE_CENTER_Y (LCD_HEIGHT / 2)

#define LCD_EYE_PCT(pct) ((int16_t)((LCD_EYE_BASE_HEIGHT * (pct)) / 100))

#define LCD_TRANSITION_DURATION_MS 300
#define LCD_TRANSITION_STEP_COUNT 9
#define LCD_TRANSITION_STEP_FACTOR (1.0f / LCD_TRANSITION_STEP_COUNT)

#define LCD_BLINK_MIN_INTERVAL_MS 2000
#define LCD_BLINK_MAX_INTERVAL_MS 5000

static const lcd_eye_config_t s_expression_presets[LCD_EXPRESSION_COUNT] = {
    {
        .offset_x              = 0,
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(100),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = 0.0f,
        .slope_bottom          = 0.0f,
        .radius_top            = LCD_EYE_PCT(15),
        .radius_bottom         = LCD_EYE_PCT(15),
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
    {
        .offset_x              = 0,
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(25),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = 0.0f,
        .slope_bottom          = 0.0f,
        .radius_top            = LCD_EYE_PCT(10),
        .radius_bottom         = 0,
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
    {
        .offset_x              = 0,
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(25),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = -0.5f,
        .slope_bottom          = 0.0f,
        .radius_top            = LCD_EYE_PCT(1),
        .radius_bottom         = LCD_EYE_PCT(10),
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
    {
        .offset_x              = 0,
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(30),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = 0.4f,
        .slope_bottom          = 0.0f,
        .radius_top            = 0,
        .radius_bottom         = LCD_EYE_PCT(10),
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
    {
        .offset_x              = -LCD_EYE_PCT(2),
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(100),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = 0.0f,
        .slope_bottom          = 0.0f,
        .radius_top            = LCD_EYE_PCT(16),
        .radius_bottom         = LCD_EYE_PCT(16),
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
    {
        .offset_x              = 0,
        .offset_y              = 0,
        .height                = LCD_EYE_PCT(15),
        .width                 = LCD_EYE_PCT(100),
        .slope_top             = 0.0f,
        .slope_bottom          = 0.0f,
        .radius_top            = 0,
        .radius_bottom         = LCD_EYE_PCT(5),
        .inverse_radius_top    = 0,
        .inverse_radius_bottom = 0,
        .inverse_offset_top    = 0,
        .inverse_offset_bottom = 0,
    },
};

static const lcd_eye_config_t s_blink_closed_config = {
    .offset_x              = 0,
    .offset_y              = 0,
    .height                = LCD_EYE_PCT(5),
    .width                 = LCD_EYE_PCT(100),
    .slope_top             = 0.0f,
    .slope_bottom          = 0.0f,
    .radius_top            = 0,
    .radius_bottom         = 0,
    .inverse_radius_top    = 0,
    .inverse_radius_bottom = 0,
    .inverse_offset_top    = 0,
    .inverse_offset_bottom = 0,
};

typedef struct
{
    lcd_eye_config_t current;
    lcd_eye_config_t target;
    lcd_expression_t target_expr;
    bool             is_transitioning;
    uint8_t          step;
    uint8_t          total_steps;
} lcd_transition_t;

typedef enum
{
    LCD_BLINK_STATE_IDLE = 0,
    LCD_BLINK_STATE_CLOSING,
    LCD_BLINK_STATE_OPENING,
} lcd_blink_state_t;

typedef struct
{
    lcd_blink_state_t state;
    uint8_t           step;
    uint8_t           total_steps;
    lcd_eye_config_t  saved_config;
    bool              auto_blink;
    uint32_t          next_blink_ms;
} lcd_blink_t;

static lcd_transition_t s_transition = {0};
static lcd_blink_t      s_blink      = {
    .state         = LCD_BLINK_STATE_IDLE,
    .step          = 0,
    .total_steps   = 3,
    .auto_blink    = true,
    .next_blink_ms = 0,
};
static bool s_initialized = false;

static int16_t lerp_int16(int16_t a, int16_t b, float t)
{
    return (int16_t)(a + (b - a) * t);
}

static float lerp_float(float a, float b, float t)
{
    return a + (b - a) * t;
}

static void lerp_eye_config(const lcd_eye_config_t* from, const lcd_eye_config_t* to, float t,
                            lcd_eye_config_t* out)
{
    out->offset_x           = lerp_int16(from->offset_x, to->offset_x, t);
    out->offset_y           = lerp_int16(from->offset_y, to->offset_y, t);
    out->height             = lerp_int16(from->height, to->height, t);
    out->width              = lerp_int16(from->width, to->width, t);
    out->slope_top          = lerp_float(from->slope_top, to->slope_top, t);
    out->slope_bottom       = lerp_float(from->slope_bottom, to->slope_bottom, t);
    out->radius_top         = lerp_int16(from->radius_top, to->radius_top, t);
    out->radius_bottom      = lerp_int16(from->radius_bottom, to->radius_bottom, t);
    out->inverse_radius_top = lerp_int16(from->inverse_radius_top, to->inverse_radius_top, t);
    out->inverse_radius_bottom =
        lerp_int16(from->inverse_radius_bottom, to->inverse_radius_bottom, t);
    out->inverse_offset_top = lerp_int16(from->inverse_offset_top, to->inverse_offset_top, t);
    out->inverse_offset_bottom =
        lerp_int16(from->inverse_offset_bottom, to->inverse_offset_bottom, t);
}

static void lcd_draw_eye(int16_t center_x, int16_t center_y, const lcd_eye_config_t* config,
                         lcd_color_t eye_color, lcd_color_t bg_color)
{
    int32_t delta_y_top    = (int32_t)(config->height * config->slope_top / 2.0f);
    int32_t delta_y_bottom = (int32_t)(config->height * config->slope_bottom / 2.0f);

    int16_t total_height = config->height + delta_y_top - delta_y_bottom;
    if (total_height <= 0)
        return;

    int16_t half_width   = config->width / 2;
    int16_t top_y        = center_y - total_height / 2 + config->offset_y;
    int16_t bottom_y     = top_y + total_height;
    int16_t center_x_adj = center_x + config->offset_x;

    for (int16_t y = top_y; y < bottom_y; y++)
    {
        float progress = (float)(y - top_y) / (float)total_height;

        int16_t margin_top = 0, margin_bottom = 0;

        if (progress < 0.5f)
        {
            float local = progress / 0.5f;
            margin_top  = (int16_t)(config->radius_top * (1.0f - local * local));
        }
        else
        {
            float local = (progress - 0.5f) / 0.5f;
            margin_bottom =
                (int16_t)(config->radius_bottom * (1.0f - (1.0f - local) * (1.0f - local)));
        }

        int16_t left_x  = center_x_adj - half_width + margin_top + margin_bottom;
        int16_t right_x = center_x_adj + half_width - margin_top - margin_bottom;

        if (left_x < right_x)
        {
            lcd_draw_hline_buffer(left_x, y, right_x - left_x + 1, eye_color);
        }
    }
}

static void lcd_draw_eyes(const lcd_eye_config_t* config, lcd_color_t eye_color,
                          lcd_color_t bg_color)
{
    int16_t left_center_x  = LCD_EYE_CENTER_X - LCD_EYE_SPACING / 2;
    int16_t right_center_x = LCD_EYE_CENTER_X + LCD_EYE_SPACING / 2;

    int16_t max_eye_width  = LCD_EYE_BASE_WIDTH * 2;
    int16_t max_eye_height = LCD_EYE_BASE_HEIGHT * 2;

    int16_t left_eye_x  = left_center_x - max_eye_width / 2;
    int16_t right_eye_x = right_center_x - max_eye_width / 2;
    int16_t eye_y       = LCD_EYE_CENTER_Y - max_eye_height / 2;

    if (left_eye_x < 0)
        left_eye_x = 0;
    if (right_eye_x < 0)
        right_eye_x = 0;
    if (eye_y < 0)
        eye_y = 0;

    int16_t eye_w = max_eye_width;
    int16_t eye_h = max_eye_height;

    if (left_eye_x + eye_w > LCD_WIDTH)
        eye_w = LCD_WIDTH - left_eye_x;
    if (right_eye_x + eye_w > LCD_WIDTH)
        eye_w = LCD_WIDTH - right_eye_x;
    if (eye_y + eye_h > LCD_HEIGHT)
        eye_h = LCD_HEIGHT - eye_y;

    lcd_fill_rect_buffer(left_eye_x, eye_y, eye_w, eye_h, bg_color);
    lcd_fill_rect_buffer(right_eye_x, eye_y, eye_w, eye_h, bg_color);

    lcd_draw_eye(left_center_x, LCD_EYE_CENTER_Y, config, eye_color, bg_color);
    lcd_draw_eye(right_center_x, LCD_EYE_CENTER_Y, config, eye_color, bg_color);
}

static void lcd_blink_update(void)
{
    if (!s_blink.auto_blink)
        return;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    switch (s_blink.state)
    {
    case LCD_BLINK_STATE_IDLE:
        if (now >= s_blink.next_blink_ms)
        {
            s_blink.saved_config = s_transition.current;
            s_blink.state        = LCD_BLINK_STATE_CLOSING;
            s_blink.step         = 0;
        }
        break;

    case LCD_BLINK_STATE_CLOSING:
        s_blink.step++;
        if (s_blink.step >= s_blink.total_steps)
        {
            s_blink.state = LCD_BLINK_STATE_OPENING;
            s_blink.step  = 0;
        }
        else
        {
            float t = (float)s_blink.step / (float)s_blink.total_steps;
            lerp_eye_config(&s_blink.saved_config, &s_blink_closed_config, t,
                            &s_transition.current);
        }
        break;

    case LCD_BLINK_STATE_OPENING:
        s_blink.step++;
        if (s_blink.step >= s_blink.total_steps)
        {
            s_transition.current = s_blink.saved_config;
            s_blink.state        = LCD_BLINK_STATE_IDLE;
            s_blink.next_blink_ms =
                now + LCD_BLINK_MIN_INTERVAL_MS +
                (esp_random() % (LCD_BLINK_MAX_INTERVAL_MS - LCD_BLINK_MIN_INTERVAL_MS));
        }
        else
        {
            float t = (float)s_blink.step / (float)s_blink.total_steps;
            lerp_eye_config(&s_blink_closed_config, &s_blink.saved_config, t,
                            &s_transition.current);
        }
        break;
    }
}

esp_err_t lcd_expression_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Expression renderer already initialized");
        return ESP_OK;
    }

    s_transition.current          = s_expression_presets[LCD_EXPRESSION_NORMAL];
    s_transition.target           = s_expression_presets[LCD_EXPRESSION_NORMAL];
    s_transition.target_expr      = LCD_EXPRESSION_NORMAL;
    s_transition.is_transitioning = false;
    s_transition.step             = 0;
    s_transition.total_steps      = LCD_TRANSITION_STEP_COUNT;

    s_blink.state         = LCD_BLINK_STATE_IDLE;
    s_blink.step          = 0;
    s_blink.total_steps   = 3;
    s_blink.auto_blink    = true;
    s_blink.next_blink_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + LCD_BLINK_MIN_INTERVAL_MS;

    s_initialized = true;
    ESP_LOGI(TAG, "Expression renderer initialized");
    return ESP_OK;
}

esp_err_t lcd_expression_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Expression renderer not initialized");
        return ESP_OK;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Expression renderer deinitialized");
    return ESP_OK;
}

void lcd_expression_update(void)
{
    if (!s_initialized)
        return;

    if (s_transition.is_transitioning)
    {
        s_transition.step++;

        if (s_transition.step >= s_transition.total_steps)
        {
            s_transition.current          = s_transition.target;
            s_transition.is_transitioning = false;
        }
        else
        {
            float t = (float)s_transition.step / (float)s_transition.total_steps;
            lerp_eye_config(&s_transition.current, &s_transition.target, t, &s_transition.current);
        }
    }

    lcd_blink_update();

    lcd_draw_eyes(&s_transition.current, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    lcd_flush_buffer();
}

esp_err_t lcd_expression_set(lcd_expression_t expr)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (expr >= LCD_EXPRESSION_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_transition.is_transitioning && s_transition.target_expr == expr)
    {
        return ESP_OK;
    }

    s_transition.target           = s_expression_presets[expr];
    s_transition.target_expr      = expr;
    s_transition.step             = 0;
    s_transition.total_steps      = LCD_TRANSITION_STEP_COUNT;
    s_transition.is_transitioning = true;

    ESP_LOGI(TAG, "Expression transition to %d", expr);
    return ESP_OK;
}

lcd_expression_t lcd_expression_get(void)
{
    return s_transition.target_expr;
}

void lcd_expression_set_auto_blink(bool enable)
{
    s_blink.auto_blink = enable;
    if (!enable)
    {
        s_blink.state = LCD_BLINK_STATE_IDLE;
    }
}

bool lcd_expression_is_auto_blink(void)
{
    return s_blink.auto_blink;
}

#endif