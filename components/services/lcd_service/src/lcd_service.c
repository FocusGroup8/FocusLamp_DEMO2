/*
 * lcd_service.c - LCD display service implementation
 * Integrates expression rendering, info display, and page management.
 */

#include "lcd_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "lcd_driver.h"
#include "device_state.h"
#include "esp_log.h"
#include "esp_random.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_service";

static bool s_initialized = false;
static bool s_sleeping = false;
static uint8_t s_brightness = 255;  /* Default max brightness */

/* ===================== Mode System & Task State ===================== */

#define LCD_MODE_NAME_MAX_LEN 20

static lcd_mode_t s_current_mode = LCD_MODE_FOCUS;
static char s_mode_name[LCD_MODE_NAME_MAX_LEN] = "专注模式";

/** @brief Focus task state */
typedef struct {
    char     name[20];
    uint32_t total_seconds;
    uint32_t remaining_seconds;
    bool     running;
    bool     completed;
    uint32_t last_update_ms;
} lcd_task_t;

static lcd_task_t s_task = {0};

/* ===================== Expression Rendering ===================== */

/* 160x60 landscape display layout (same as lcd folder project) */
#define LCD_EYE_BASE_WIDTH 32
#define LCD_EYE_BASE_HEIGHT 32
#define LCD_EYE_SPACING 48
#define LCD_EYE_CENTER_X (LCD_WIDTH / 2)
#define LCD_EYE_CENTER_Y (LCD_HEIGHT / 2)

#define LCD_EYE_PCT(pct) ((int16_t)((LCD_EYE_BASE_HEIGHT * (pct)) / 100))

#define LCD_TRANSITION_STEP_COUNT 9
#define LCD_TRANSITION_STEP_FACTOR (1.0f / LCD_TRANSITION_STEP_COUNT)

#define LCD_BLINK_MIN_INTERVAL_MS 2000
#define LCD_BLINK_MAX_INTERVAL_MS 5000

static const lcd_eye_config_t s_expression_presets[LCD_EXPRESSION_COUNT] = {
    /* LCD_EXPRESSION_NORMAL */
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
    /* LCD_EXPRESSION_HAPPY */
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
    /* LCD_EXPRESSION_SAD */
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
    /* LCD_EXPRESSION_ANGRY */
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
    /* LCD_EXPRESSION_SURPRISED */
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
    /* LCD_EXPRESSION_SLEEPY */
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

typedef struct {
    lcd_eye_config_t current;
    lcd_eye_config_t target;
    lcd_eye_config_t from;
    lcd_expression_t target_expr;
    bool             is_transitioning;
    uint8_t          step;
    uint8_t          total_steps;
} lcd_transition_t;

typedef enum {
    LCD_BLINK_STATE_IDLE = 0,
    LCD_BLINK_STATE_CLOSING,
    LCD_BLINK_STATE_OPENING,
} lcd_blink_state_t;

typedef struct {
    lcd_blink_state_t state;
    uint8_t           step;
    uint8_t           total_steps;
    lcd_eye_config_t  saved_config;
    bool              auto_blink;
    uint32_t          next_blink_ms;
} lcd_blink_t;

static lcd_transition_t s_transition = {0};
static lcd_blink_t s_blink = {
    .state         = LCD_BLINK_STATE_IDLE,
    .step          = 0,
    .total_steps   = 3,
    .auto_blink    = true,
    .next_blink_ms = 0,
};
static bool s_expr_initialized = false;

static int16_t lerp_int16(int16_t a, int16_t b, float t)
{
    return (int16_t)(a + (b - a) * t);
}

static float lerp_float(float a, float b, float t)
{
    return a + (b - a) * t;
}

static void lerp_eye_config(const lcd_eye_config_t *from, const lcd_eye_config_t *to, float t,
                            lcd_eye_config_t *out)
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
    out->inverse_radius_bottom = lerp_int16(from->inverse_radius_bottom, to->inverse_radius_bottom, t);
    out->inverse_offset_top = lerp_int16(from->inverse_offset_top, to->inverse_offset_top, t);
    out->inverse_offset_bottom = lerp_int16(from->inverse_offset_bottom, to->inverse_offset_bottom, t);
}

static void lcd_draw_eye(int16_t center_x, int16_t center_y, const lcd_eye_config_t *config,
                         lcd_color_t eye_color, lcd_color_t bg_color)
{
    (void)bg_color;
    int32_t delta_y_top    = (int32_t)(config->height * config->slope_top / 2.0f);
    int32_t delta_y_bottom = (int32_t)(config->height * config->slope_bottom / 2.0f);

    int16_t total_height = config->height + delta_y_top - delta_y_bottom;
    if (total_height <= 0)
        return;

    int16_t half_width   = config->width / 2;
    int16_t top_y        = center_y - total_height / 2 + config->offset_y;
    int16_t bottom_y     = top_y + total_height;
    int16_t center_x_adj = center_x + config->offset_x;

    for (int16_t y = top_y; y < bottom_y; y++) {
        float progress = (float)(y - top_y) / (float)total_height;

        int16_t margin_top = 0, margin_bottom = 0;

        if (progress < 0.5f) {
            float local = progress / 0.5f;
            margin_top  = (int16_t)(config->radius_top * (1.0f - local * local));
        } else {
            float local = (progress - 0.5f) / 0.5f;
            margin_bottom = (int16_t)(config->radius_bottom * (1.0f - (1.0f - local) * (1.0f - local)));
        }

        int16_t left_x  = center_x_adj - half_width + margin_top + margin_bottom;
        int16_t right_x = center_x_adj + half_width - margin_top - margin_bottom;

        if (left_x < right_x) {
            lcd_draw_hline_buffer(left_x, y, right_x - left_x + 1, eye_color);
        }
    }
}

static void lcd_draw_eyes(const lcd_eye_config_t *config, lcd_color_t eye_color,
                          lcd_color_t bg_color)
{
    int16_t left_center_x  = LCD_EYE_CENTER_X - LCD_EYE_SPACING / 2;
    int16_t right_center_x = LCD_EYE_CENTER_X + LCD_EYE_SPACING / 2;

    int16_t max_eye_width  = LCD_EYE_BASE_WIDTH * 2;
    int16_t max_eye_height = LCD_EYE_BASE_HEIGHT * 2;

    int16_t left_eye_x  = left_center_x - max_eye_width / 2;
    int16_t right_eye_x = right_center_x - max_eye_width / 2;
    int16_t eye_y       = LCD_EYE_CENTER_Y - max_eye_height / 2;

    if (left_eye_x < 0) left_eye_x = 0;
    if (right_eye_x < 0) right_eye_x = 0;
    if (eye_y < 0) eye_y = 0;

    int16_t eye_w = max_eye_width;
    int16_t eye_h = max_eye_height;

    if (left_eye_x + eye_w > LCD_WIDTH)  eye_w = LCD_WIDTH - left_eye_x;
    if (right_eye_x + eye_w > LCD_WIDTH) eye_w = LCD_WIDTH - right_eye_x;
    if (eye_y + eye_h > LCD_HEIGHT) eye_h = LCD_HEIGHT - eye_y;

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

    switch (s_blink.state) {
    case LCD_BLINK_STATE_IDLE:
        if (now >= s_blink.next_blink_ms) {
            s_blink.saved_config = s_transition.current;
            s_blink.state        = LCD_BLINK_STATE_CLOSING;
            s_blink.step         = 0;
        }
        break;

    case LCD_BLINK_STATE_CLOSING:
        s_blink.step++;
        if (s_blink.step >= s_blink.total_steps) {
            s_blink.state = LCD_BLINK_STATE_OPENING;
            s_blink.step  = 0;
        } else {
            float t = (float)s_blink.step / (float)s_blink.total_steps;
            lerp_eye_config(&s_blink.saved_config, &s_blink_closed_config, t,
                            &s_transition.current);
        }
        break;

    case LCD_BLINK_STATE_OPENING:
        s_blink.step++;
        if (s_blink.step >= s_blink.total_steps) {
            s_transition.current = s_blink.saved_config;
            s_blink.state        = LCD_BLINK_STATE_IDLE;
            s_blink.next_blink_ms =
                now + LCD_BLINK_MIN_INTERVAL_MS +
                (esp_random() % (LCD_BLINK_MAX_INTERVAL_MS - LCD_BLINK_MIN_INTERVAL_MS));
        } else {
            float t = (float)s_blink.step / (float)s_blink.total_steps;
            lerp_eye_config(&s_blink_closed_config, &s_blink.saved_config, t,
                            &s_transition.current);
        }
        break;
    }
}

/* ===================== Expression API Implementation ===================== */

esp_err_t lcd_service_expression_init(void)
{
    if (s_expr_initialized) {
        ESP_LOGW(TAG, "Expression renderer already initialized");
        return ESP_OK;
    }

    s_transition.current          = s_expression_presets[LCD_EXPRESSION_NORMAL];
    s_transition.target           = s_expression_presets[LCD_EXPRESSION_NORMAL];
    s_transition.from             = s_expression_presets[LCD_EXPRESSION_NORMAL];
    s_transition.target_expr      = LCD_EXPRESSION_NORMAL;
    s_transition.is_transitioning = false;
    s_transition.step             = 0;
    s_transition.total_steps      = LCD_TRANSITION_STEP_COUNT;

    s_blink.state         = LCD_BLINK_STATE_IDLE;
    s_blink.step          = 0;
    s_blink.total_steps   = 3;
    s_blink.auto_blink    = true;
    s_blink.next_blink_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + LCD_BLINK_MIN_INTERVAL_MS;

    s_expr_initialized = true;
    ESP_LOGI(TAG, "Expression renderer initialized");
    return ESP_OK;
}

esp_err_t lcd_service_expression_deinit(void)
{
    if (!s_expr_initialized) {
        ESP_LOGW(TAG, "Expression renderer not initialized");
        return ESP_OK;
    }

    s_expr_initialized = false;
    ESP_LOGI(TAG, "Expression renderer deinitialized");
    return ESP_OK;
}

void lcd_service_expression_update(void)
{
    if (!s_expr_initialized)
        return;

    /* Handle blink animation */
    lcd_blink_update();

    /* Handle expression transition */
    if (s_transition.is_transitioning) {
        s_transition.step++;
        if (s_transition.step >= s_transition.total_steps) {
            s_transition.current        = s_transition.target;
            s_transition.is_transitioning = false;
        } else {
            float t = (float)(s_transition.step) * LCD_TRANSITION_STEP_FACTOR;
            lerp_eye_config(&s_transition.from, &s_transition.target, t, &s_transition.current);
        }
    }

    lcd_fill_rect_buffer(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COLOR_BLACK);
    lcd_draw_eyes(&s_transition.current, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    /* Diagnostic: white top+bottom border to verify expression_update runs */
    lcd_draw_hline_buffer(0, 0, LCD_WIDTH, LCD_COLOR_WHITE);
    lcd_draw_hline_buffer(0, LCD_HEIGHT - 1, LCD_WIDTH, LCD_COLOR_WHITE);
    /* 不在本函数内 flush：由调用方统一 flush，避免叠加文本造成闪烁 */
}

esp_err_t lcd_service_expression_set(lcd_expression_t expr)
{
    if (!s_expr_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (expr >= LCD_EXPRESSION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_transition.is_transitioning && s_transition.target_expr == expr) {
        return ESP_OK;
    }

    s_transition.from             = s_transition.current;
    s_transition.target           = s_expression_presets[expr];
    s_transition.target_expr      = expr;
    s_transition.step             = 0;
    s_transition.total_steps      = LCD_TRANSITION_STEP_COUNT;
    s_transition.is_transitioning = true;

    ESP_LOGI(TAG, "Expression transition to %d", expr);

    return ESP_OK;
}

lcd_expression_t lcd_service_expression_get(void)
{
    return s_transition.target_expr;
}

void lcd_service_expression_set_auto_blink(bool enable)
{
    s_blink.auto_blink = enable;
    if (!enable) {
        s_blink.state = LCD_BLINK_STATE_IDLE;
    }
}

bool lcd_service_expression_is_auto_blink(void)
{
    return s_blink.auto_blink;
}

/* ===================== Info Display Implementation ===================== */

#define LCD_INFO_TITLE_MAX_LEN 16
#define LCD_INFO_TIMER_MAX_LEN 12

/* Layout for 160x60 landscape: 3 rows
   Row1 (Y=0) : mode name + task title
   Row2       : progress bar (left) + timer (right, right-aligned)
   Row3 (Y=44): heart rate */
#define LCD_INFO_MODE_X 4            /* Mode name X */
#define LCD_INFO_MODE_Y 0            /* Row 1: mode/task row */
#define LCD_INFO_TASK_GAP 20         /* Gap between mode name and task title (≈2 ASCII spaces) */

#define LCD_INFO_BAR_X 4            /* Row 2: progress bar X (left-aligned) */
#define LCD_INFO_BAR_Y 23           /* Row 2: progress bar Y (14px high, centered with timer) */
#define LCD_INFO_BAR_HEIGHT 14
#define LCD_INFO_TIMER_SIZE 1   /* 8px per char; right-aligned */

#define LCD_INFO_HR_Y 44            /* Row 3: heart rate row (Chinese font 16px high) */
#define LCD_INFO_MODE_BAND_H 16     /* Mode name overlay band height (16px) */

typedef struct {
    char               task_title[LCD_INFO_TITLE_MAX_LEN];
    uint32_t           timer_seconds;
    bool               initialized;
} lcd_info_state_t;

static lcd_info_state_t s_info_state = {
    .task_title    = "Idle",
    .timer_seconds = 0,
    .initialized   = false,
};

/* ===================== Mode System Implementation ===================== */

void lcd_service_mode_set(lcd_mode_t mode)
{
    /* Wake up LCD if sleeping, so mode change is visible */
    lcd_service_wakeup();

    s_current_mode = mode;
    switch (mode) {
    case LCD_MODE_FOCUS:
        lcd_service_set_companion_overlay(false);
        lcd_service_page_switch_to(LCD_PAGE_INFO);
        strncpy(s_mode_name, "专注模式", sizeof(s_mode_name) - 1);
        break;
    case LCD_MODE_COMPANION:
        /* 陪伴模式：表情页显示开心眼睛，叠加模式名+心率 */
        lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
        lcd_service_expression_set(LCD_EXPRESSION_HAPPY);
        lcd_service_set_companion_overlay(true);
        strncpy(s_mode_name, "陪伴模式", sizeof(s_mode_name) - 1);
        break;
    case LCD_MODE_SILENT:
        lcd_service_set_companion_overlay(false);
        lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
        lcd_service_expression_set(LCD_EXPRESSION_SLEEPY);
        strncpy(s_mode_name, "静默模式", sizeof(s_mode_name) - 1);
        break;
    case LCD_MODE_CUSTOM:
        lcd_service_set_companion_overlay(false);
        lcd_service_page_switch_to(LCD_PAGE_INFO);
        break;
    }
    s_mode_name[sizeof(s_mode_name) - 1] = '\0';
    ESP_LOGI(TAG, "Mode: %s", s_mode_name);
}

lcd_mode_t lcd_service_mode_get(void)
{
    return s_current_mode;
}

void lcd_service_mode_set_name(const char *name)
{
    if (name) {
        strncpy(s_mode_name, name, sizeof(s_mode_name) - 1);
        s_mode_name[sizeof(s_mode_name) - 1] = '\0';
    }
}

/* ===================== Task Management Implementation ===================== */

void lcd_service_task_start(const char *name, uint32_t seconds)
{
    /* Wake up LCD if sleeping, so task display is visible */
    lcd_service_wakeup();

    if (name) {
        strncpy(s_task.name, name, sizeof(s_task.name) - 1);
        s_task.name[sizeof(s_task.name) - 1] = '\0';
    }
    s_task.total_seconds = seconds;
    s_task.remaining_seconds = seconds;
    s_task.running = true;
    s_task.completed = false;
    s_task.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Switch to FOCUS mode first (this triggers page switch, resets info) */
    lcd_service_mode_set(LCD_MODE_FOCUS);

    /* Then set actual task info (overrides "Idle" set by page switch) */
    lcd_service_info_set_title(s_task.name);
    lcd_service_info_set_timer(seconds);

    ESP_LOGI(TAG, "Task started: '%s' %" PRIu32 " seconds", s_task.name, seconds);
}

void lcd_service_task_stop(void)
{
    /* Wake up LCD if sleeping */
    lcd_service_wakeup();

    s_task.running = false;
    s_task.completed = false;
    s_task.remaining_seconds = 0;

    lcd_service_info_set_title("Idle");
    lcd_service_info_set_timer(0);

    ESP_LOGI(TAG, "Task stopped");
}

bool lcd_service_task_is_running(void)
{
    return s_task.running;
}

/* ===================== Info Display Helpers ===================== */

static void format_timer(uint32_t seconds, char *buffer, size_t buffer_size)
{
    uint32_t minutes = seconds / 60;
    uint32_t secs    = seconds % 60;
    snprintf(buffer, buffer_size, "%02" PRIu32 ":%02" PRIu32, minutes, secs);
}

/* 计算 UTF-8 字符串像素宽度：中文固定 16px，ASCII 为 8*ascii_size */
static uint16_t utf8_string_width_px(const char *str, uint8_t ascii_size)
{
    uint16_t w = 0;
    if (str == NULL) {
        return 0;
    }
    while (*str) {
        uint8_t b0 = (uint8_t)*str;
        if (b0 >= 0xE0 && b0 <= 0xEF && str[1] && str[2]) {
            w += 16;
            str += 3;
        } else if (b0 >= 0x80) {
            str++;  /* unsupported multibyte, skip */
        } else {
            w += 8 * ascii_size;
            str++;
        }
    }
    return w;
}

/* 在 max_px 宽度内绘制 UTF-8 字符串，超出部分截断 */
static void draw_utf8_truncated(uint16_t x, uint16_t y, const char *str, uint16_t max_px,
                                lcd_color_t color, lcd_color_t bg, uint8_t ascii_size)
{
    if (str == NULL || max_px == 0) {
        return;
    }
    uint16_t cx   = x;
    uint16_t avail = max_px;
    while (*str) {
        uint8_t b0   = (uint8_t)*str;
        uint16_t cw;
        bool     is_cn;
        if (b0 >= 0xE0 && b0 <= 0xEF && str[1] && str[2]) {
            cw    = 16;
            is_cn = true;
        } else if (b0 >= 0x80) {
            str++;
            continue;
        } else {
            cw    = 8 * ascii_size;
            is_cn = false;
        }
        if (cw > avail) {
            break;  /* truncated */
        }
        lcd_draw_utf8_string_buffer(cx, y, str, color, bg, ascii_size);
        avail -= cw;
        cx    += cw;
        str   += is_cn ? 3 : 1;
    }
}

esp_err_t lcd_service_info_init(void)
{
    if (s_info_state.initialized) {
        ESP_LOGW(TAG, "Info display already initialized");
        return ESP_OK;
    }

    strncpy(s_info_state.task_title, "Idle", LCD_INFO_TITLE_MAX_LEN - 1);
    s_info_state.task_title[LCD_INFO_TITLE_MAX_LEN - 1] = '\0';
    s_info_state.timer_seconds = 0;
    s_info_state.initialized   = true;

    ESP_LOGI(TAG, "Info display initialized");
    return ESP_OK;
}

esp_err_t lcd_service_info_deinit(void)
{
    if (!s_info_state.initialized) {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_OK;
    }

    s_info_state.initialized = false;
    ESP_LOGI(TAG, "Info display deinitialized");
    return ESP_OK;
}

esp_err_t lcd_service_info_set_title(const char *title)
{
    if (!s_info_state.initialized) {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (title == NULL) {
        ESP_LOGE(TAG, "Title is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_info_state.task_title, title, LCD_INFO_TITLE_MAX_LEN - 1);
    s_info_state.task_title[LCD_INFO_TITLE_MAX_LEN - 1] = '\0';

    ESP_LOGD(TAG, "Title set to: %s", s_info_state.task_title);
    return ESP_OK;
}

esp_err_t lcd_service_info_set_timer(uint32_t seconds)
{
    if (!s_info_state.initialized) {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_info_state.timer_seconds = seconds;

    ESP_LOGD(TAG, "Timer set to: %" PRIu32 " seconds", seconds);
    return ESP_OK;
}

/* ===================== Heart Rate Display & Companion Overlay ===================== */

static bool s_companion_overlay = false;

/**
 * @brief 绘制心率文本到指定行（左对齐，中文"心 率：xx bpm"，无效时显示"心 率：-- bpm"）
 */
static void lcd_service_draw_heart_rate(uint16_t y)
{
    char hr_str[24];
    device_state_t state = {0};
    device_state_get(&state);

    if (state.radar.heart_rate_valid && state.radar.heart_rate_bpm > 0.0f) {
        snprintf(hr_str, sizeof(hr_str), "心 率：%ubpm",
                 (unsigned)(state.radar.heart_rate_bpm + 0.5f));
    } else {
        snprintf(hr_str, sizeof(hr_str), "心 率：--bpm");
    }
    lcd_draw_utf8_string_buffer(LCD_INFO_MODE_X, y, hr_str, LCD_COLOR_WHITE,
                                LCD_COLOR_BLACK, 1);
}

void lcd_service_set_companion_overlay(bool enable)
{
    if (s_companion_overlay == enable) {
        return;
    }
    s_companion_overlay = enable;

    if (!enable) {
        /* 退出陪伴模式时清除表情页残留叠加文本 */
        lcd_fill_rect_buffer(0, LCD_INFO_MODE_Y, LCD_WIDTH, LCD_INFO_MODE_BAND_H,
                             LCD_COLOR_BLACK);
        lcd_fill_rect_buffer(0, LCD_INFO_HR_Y, LCD_WIDTH,
                             LCD_HEIGHT - LCD_INFO_HR_Y, LCD_COLOR_BLACK);
        lcd_flush_buffer();
    }
    ESP_LOGI(TAG, "Companion overlay %s", enable ? "enabled" : "disabled");
}

/**
 * @brief 陪伴模式表情页叠加：顶部模式名 + 底部心率（保留开心眼睛）
 */
static void lcd_service_draw_companion_overlay(void)
{
    lcd_draw_utf8_string_buffer(LCD_INFO_MODE_X, LCD_INFO_MODE_Y, "陪伴模式",
                                LCD_COLOR_WHITE, LCD_COLOR_BLACK, 1);
    lcd_service_draw_heart_rate(LCD_INFO_HR_Y);
    /* 不在此 flush：与表情帧统一由调用方 flush，避免两帧交替闪烁 */
}

void lcd_service_info_update(void)
{
    if (!s_info_state.initialized) {
        return;
    }

    lcd_fill_rect_buffer(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COLOR_BLACK);

    /* ---- Row 1: mode name (left) + task title (after mode name) ---- */
    lcd_draw_utf8_string_buffer(LCD_INFO_MODE_X, LCD_INFO_MODE_Y, s_mode_name, LCD_COLOR_WHITE,
                                LCD_COLOR_BLACK, 1);
    uint16_t mode_w = utf8_string_width_px(s_mode_name, 1);
    uint16_t task_x = LCD_INFO_MODE_X + mode_w + LCD_INFO_TASK_GAP;
    uint16_t task_w = (task_x < (uint16_t)(LCD_WIDTH - 4)) ? (LCD_WIDTH - 4 - task_x) : 0;
    draw_utf8_truncated(task_x, LCD_INFO_MODE_Y, s_info_state.task_title, task_w,
                        LCD_COLOR_WHITE, LCD_COLOR_BLACK, 1);

    /* ---- Row 2: timer / "完成" (right-aligned) + progress bar (left) ---- */
    char   timer_str[LCD_INFO_TIMER_MAX_LEN];
    bool   done = s_task.completed;
    /* 与进度条垂直居中：中文"完成"16px，倒计时8px */
    uint16_t timer_y = LCD_INFO_BAR_Y + LCD_INFO_BAR_HEIGHT / 2 - (done ? 8 : 4);

    uint16_t right_w = 0;
    if (done) {
        right_w = 32;  /* "完成" 两个字宽 */
    } else {
        format_timer(s_info_state.timer_seconds, timer_str, sizeof(timer_str));
        right_w = (uint16_t)(strlen(timer_str) * 8 * LCD_INFO_TIMER_SIZE);
    }
    uint16_t right_x = (right_w + 4 < LCD_WIDTH) ? (LCD_WIDTH - right_w - 4) : LCD_INFO_BAR_X;
    uint16_t bar_w   = (right_x > (uint16_t)(LCD_INFO_BAR_X + LCD_INFO_TASK_GAP)) ?
                       (right_x - LCD_INFO_BAR_X - LCD_INFO_TASK_GAP) : 0;

    if (done) {
        lcd_draw_utf8_string_buffer(right_x, timer_y, "完成", LCD_COLOR_WHITE,
                                    LCD_COLOR_BLACK, 1);
    } else {
        lcd_draw_string_buffer(right_x, timer_y, timer_str, LCD_COLOR_WHITE,
                               LCD_COLOR_BLACK, LCD_INFO_TIMER_SIZE);
    }

    uint16_t fill_width = 0;
    if (s_task.running && s_task.total_seconds > 0) {
        uint32_t elapsed = s_task.total_seconds - s_task.remaining_seconds;
        fill_width = (uint16_t)((uint32_t)bar_w * elapsed / s_task.total_seconds);
        if (fill_width > bar_w) fill_width = bar_w;
    } else if (done) {
        fill_width = bar_w;  /* 任务完成：进度条满格 */
    }
    if (fill_width > 0) {
        /* Background */
        lcd_fill_rect_buffer(LCD_INFO_BAR_X, LCD_INFO_BAR_Y, bar_w, LCD_INFO_BAR_HEIGHT, 0x3165);
        /* Fill */
        lcd_fill_rect_buffer(LCD_INFO_BAR_X, LCD_INFO_BAR_Y, fill_width, LCD_INFO_BAR_HEIGHT, LCD_COLOR_WHITE);
    }

    /* ---- Row 3: heart rate (left-aligned) ---- */
    lcd_service_draw_heart_rate(LCD_INFO_HR_Y);

    lcd_flush_buffer();
}

/* ===================== Page Management Implementation ===================== */

typedef void (*lcd_page_switch_cb_t)(lcd_page_t page);

typedef struct {
    lcd_page_t           current_page;
    lcd_page_switch_cb_t on_page_switch;
    bool                 initialized;
} lcd_page_manager_t;

static lcd_page_manager_t s_page_manager = {
    .current_page   = LCD_PAGE_EXPRESSION,
    .on_page_switch = NULL,
    .initialized    = false,
};

esp_err_t lcd_service_page_init(void)
{
    if (s_page_manager.initialized) {
        ESP_LOGW(TAG, "Page manager already initialized");
        return ESP_OK;
    }

    s_page_manager.current_page   = LCD_PAGE_EXPRESSION;
    s_page_manager.on_page_switch = NULL;
    s_page_manager.initialized    = true;

    ESP_LOGI(TAG, "Page manager initialized");
    return ESP_OK;
}

esp_err_t lcd_service_page_deinit(void)
{
    if (!s_page_manager.initialized) {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_OK;
    }

    s_page_manager.current_page   = LCD_PAGE_EXPRESSION;
    s_page_manager.on_page_switch = NULL;
    s_page_manager.initialized    = false;

    ESP_LOGI(TAG, "Page manager deinitialized");
    return ESP_OK;
}

esp_err_t lcd_service_page_switch_to(lcd_page_t page)
{
    if (!s_page_manager.initialized) {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (page >= LCD_PAGE_COUNT) {
        ESP_LOGE(TAG, "Invalid page: %d", page);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_page_manager.current_page == page) {
        ESP_LOGD(TAG, "Already on page %d", page);
        return ESP_OK;
    }

    s_page_manager.current_page = page;

    ESP_LOGI(TAG, "Switched to page %d", page);

    if (s_page_manager.on_page_switch != NULL) {
        s_page_manager.on_page_switch(page);
    }

    /* Publish page change event */
    event_bus_publish_simple(EV_LCD_PAGE_CHANGE);

    return ESP_OK;
}

esp_err_t lcd_service_page_next(void)
{
    if (!s_page_manager.initialized) {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    lcd_page_t next_page = (lcd_page_t)((s_page_manager.current_page + 1) % LCD_PAGE_COUNT);
    return lcd_service_page_switch_to(next_page);
}

lcd_page_t lcd_service_page_get_current(void)
{
    return s_page_manager.current_page;
}

void lcd_service_page_register_callback(void (*callback)(lcd_page_t page))
{
    s_page_manager.on_page_switch = callback;
}

/* ===================== Unified Update API ===================== */

void lcd_service_update(void)
{
    if (!s_initialized || s_sleeping) {
        return;
    }

    /* Task timer countdown */
    if (s_task.running) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t elapsed_ms = now_ms - s_task.last_update_ms;
        if (elapsed_ms >= 1000) {
            uint32_t elapsed_sec = elapsed_ms / 1000;
            s_task.last_update_ms += elapsed_sec * 1000;

            if (s_task.remaining_seconds > elapsed_sec) {
                s_task.remaining_seconds -= elapsed_sec;
            } else {
                s_task.remaining_seconds = 0;
                s_task.running = false;
                s_task.completed = true;
                ESP_LOGI(TAG, "Task '%s' completed!", s_task.name);
            }
            lcd_service_info_set_timer(s_task.remaining_seconds);
        }
    }

    lcd_page_t page = lcd_service_page_get_current();

    if (page == LCD_PAGE_EXPRESSION) {
        lcd_service_expression_update();
        if (s_companion_overlay) {
            lcd_service_draw_companion_overlay();
        }
        /* 表情+叠加文本绘制完毕后统一刷新上屏，避免闪烁 */
        lcd_flush_buffer();
    } else if (page == LCD_PAGE_INFO) {
        lcd_service_info_update();
    }
}

/* ===================== Event Callbacks ===================== */

static void on_page_switch(lcd_page_t page)
{
    ESP_LOGI(TAG, "Native page switched to %d", page);

    if (page == LCD_PAGE_EXPRESSION) {
        lcd_service_expression_set(LCD_EXPRESSION_NORMAL);
    } else if (page == LCD_PAGE_INFO) {
        lcd_service_info_init();
    }
}

static void lcd_service_event_handler(event_t *event, void *context)
{
    switch (event->type) {
    case EV_LCD_UPDATE:
        /* Refresh current native page display */
        {
            lcd_page_t current = lcd_service_page_get_current();
            if (current == LCD_PAGE_EXPRESSION) {
                lcd_service_expression_update();
                if (s_companion_overlay) {
                    lcd_service_draw_companion_overlay();
                }
                /* 表情+叠加文本绘制完毕后统一刷新上屏，避免闪烁 */
                lcd_flush_buffer();
            } else if (current == LCD_PAGE_INFO) {
                lcd_service_info_update();
            }
        }
        break;
    case EV_LCD_PAGE_CHANGE:
        /* Handled by page manager callback */
        break;
    case EV_LCD_SLEEP:
        lcd_service_sleep();
        break;
    case EV_LCD_WAKEUP:
        lcd_service_wakeup();
        break;
    case EV_LCD_BRIGHTNESS_CHANGED:
        if (event->data != NULL && event->data_size == sizeof(uint8_t)) {
            uint8_t brightness = *(const uint8_t *)event->data;
            if (brightness != s_brightness) {
                lcd_service_set_brightness(brightness);
                ESP_LOGI(TAG, "Brightness changed to %u via event", brightness);
            }
        } else {
            ESP_LOGW(TAG, "Invalid brightness event data");
        }
        break;
    case EV_LCD_TIMEOUT:
        lcd_service_sleep();
        break;
    default:
        break;
    }
}

/* ===================== Public API ===================== */

esp_err_t lcd_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing LCD service...");

    esp_err_t ret = lcd_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD driver init failed");
        return ret;
    }

    /* Initialize native page manager */
    ret = lcd_service_page_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Page manager init failed");
        return ret;
    }

    /* Register page switch callback */
    lcd_service_page_register_callback(on_page_switch);

    /* Initialize expression renderer (default page) */
    ret = lcd_service_expression_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Expression renderer init failed");
        return ret;
    }

    /* Initialize info display */
    ret = lcd_service_info_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Info display init failed");
        return ret;
    }

    /* Subscribe to LCD events */
    event_bus_subscribe(EV_LCD_UPDATE, lcd_service_event_handler, NULL);
    event_bus_subscribe(EV_LCD_PAGE_CHANGE, lcd_service_event_handler, NULL);
    event_bus_subscribe(EV_LCD_SLEEP, lcd_service_event_handler, NULL);
    event_bus_subscribe(EV_LCD_WAKEUP, lcd_service_event_handler, NULL);
    event_bus_subscribe(EV_LCD_BRIGHTNESS_CHANGED, lcd_service_event_handler, NULL);
    event_bus_subscribe(EV_LCD_TIMEOUT, lcd_service_event_handler, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "LCD service initialized");
    return ESP_OK;
}

void lcd_service_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return;
    }

    s_brightness = brightness;
    lcd_driver_set_backlight(brightness);

    event_t ev = {
        .type = EV_LCD_BRIGHTNESS_CHANGED,
        .data = &brightness,
        .data_size = sizeof(brightness),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
}

void lcd_service_sleep(void)
{
    if (!s_initialized || s_sleeping) {
        return;
    }

    ESP_LOGI(TAG, "LCD sleep");
    s_sleeping = true;
    lcd_driver_display_off();

    event_bus_publish_simple(EV_LCD_SLEEP);
}

void lcd_service_wakeup(void)
{
    if (!s_initialized || !s_sleeping) {
        return;
    }

    ESP_LOGI(TAG, "LCD wakeup");
    s_sleeping = false;
    lcd_driver_display_on();
    lcd_driver_set_backlight(s_brightness);

    event_bus_publish_simple(EV_LCD_WAKEUP);
}
