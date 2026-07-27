#include "lvgl_ui.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1)

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "event_bus.h"
#include "event_data.h"
#include "lcd_driver.h"
#include "lvgl.h"
#include "lvgl_disp.h"

#if (LVGL_UI_ENABLE_EXPRESSION == 1)
#include "lvgl_expression.h"
#endif

#if (LVGL_UI_ENABLE_INFO == 1)
#include "lvgl_info.h"
#endif

#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
#include "lvgl_radar_view.h"
#endif

#if (LVGL_UI_ENABLE_ENCODER == 1)
#include "driver/gpio.h"
#endif

static const char* TAG = "LVGL_UI";

static SemaphoreHandle_t s_lvgl_mutex = NULL;

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef struct
{
    bool                     initialized;
    lvgl_ui_page_t           current_page;
    lvgl_ui_event_callback_t event_callback;
    lv_obj_t*                main_screen;
} lvgl_ui_t;

static lvgl_ui_t s_lvgl_ui = {
    .initialized    = false,
    .current_page   = LVGL_UI_PAGE_EXPRESSION,
    .event_callback = NULL,
    .main_screen    = NULL,
};

#if (LVGL_UI_ENABLE_ENCODER == 1)
static lv_indev_t* s_encoder_indev   = NULL;
static int32_t     s_encoder_diff    = 0;
static bool        s_encoder_pressed = false;

static void encoder_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;
    data->enc_diff = s_encoder_diff;
    s_encoder_diff = 0;
    data->state    = s_encoder_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void encoder_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LVGL_UI_ENCODER_A_GPIO) | (1ULL << LVGL_UI_ENCODER_B_GPIO) |
                        (1ULL << LVGL_UI_ENCODER_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    lv_display_t* disp = lvgl_disp_get();
    if (disp == NULL)
    {
        ESP_LOGW(TAG, "Display not ready for encoder");
        return;
    }

    s_encoder_indev = lv_indev_create_for_display(disp);
    if (s_encoder_indev == NULL)
    {
        ESP_LOGW(TAG, "Failed to create encoder input device");
        return;
    }
    lv_indev_set_type(s_encoder_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_encoder_indev, encoder_read_cb);
}
#endif

static void unload_current_page(void)
{
    switch (s_lvgl_ui.current_page)
    {
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    case LVGL_UI_PAGE_EXPRESSION:
        lvgl_expression_deinit();
        break;
#endif
#if (LVGL_UI_ENABLE_INFO == 1)
    case LVGL_UI_PAGE_INFO:
        lvgl_info_deinit();
        break;
#endif
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    case LVGL_UI_PAGE_RADAR_VIEW:
        lvgl_radar_view_deinit();
        break;
#endif
    default:
        break;
    }
}

static void load_page(lvgl_ui_page_t page)
{
    if (page < 0 || page >= LVGL_UI_PAGE_COUNT)
    {
        ESP_LOGW(TAG, "Invalid page: %d", page);
        return;
    }

    ESP_LOGI(TAG, "Loading page %d", page);

    if (s_lvgl_mutex == NULL)
    {
        ESP_LOGE(TAG, "LVGL mutex is NULL");
        return;
    }

    ESP_LOGD(TAG, "Attempting to take LVGL mutex for page switch");
    TickType_t start_tick = xTaskGetTickCount();

    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take LVGL mutex after 1000ms timeout");
        ESP_LOGE(TAG, "Mutex may be held by LVGL task or other operation");
        return;
    }

    TickType_t end_tick = xTaskGetTickCount();
    ESP_LOGD(TAG, "Mutex acquired successfully in %d ms",
             (end_tick - start_tick) * portTICK_PERIOD_MS);

    unload_current_page();

    ESP_LOGI(TAG, "Unloaded current page");

    s_lvgl_ui.current_page = page;

    lv_obj_t* active_screen = s_lvgl_ui.main_screen;
    if (active_screen == NULL)
    {
        ESP_LOGE(TAG, "Main screen is NULL");
        xSemaphoreGive(s_lvgl_mutex);
        return;
    }

    ESP_LOGD(TAG, "Using main screen: %p", active_screen);

    switch (page)
    {
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    case LVGL_UI_PAGE_EXPRESSION:
        ESP_LOGI(TAG, "Initializing expression page");
        lvgl_expression_init(active_screen);
        break;
#endif
#if (LVGL_UI_ENABLE_INFO == 1)
    case LVGL_UI_PAGE_INFO:
        ESP_LOGI(TAG, "Initializing info page");
        lvgl_info_init(active_screen);
        break;
#endif
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    case LVGL_UI_PAGE_RADAR_VIEW:
        ESP_LOGI(TAG, "Initializing radar page");
        lvgl_radar_view_init(active_screen);
        break;
#endif
    default:
        break;
    }

    ESP_LOGI(TAG, "Page %d loaded successfully", page);

#if (LVGL_UI_ENABLE_ENCODER == 1)
    if (s_encoder_indev != NULL)
    {
        lv_indev_set_group(s_encoder_indev, lv_group_get_default());
    }
#endif

    ESP_LOGD(TAG, "Releasing LVGL mutex");
    xSemaphoreGive(s_lvgl_mutex);
    ESP_LOGD(TAG, "LVGL mutex released");
}

static void radar_event_handler(const event_t* event, void* user_data)
{
    (void)user_data;

    if (event == NULL || event->type != EVENT_TYPE_RADAR_DATA)
    {
        return;
    }

    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to take LVGL mutex in event handler");
        return;
    }

    switch (event->id)
    {
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    case RADAR_EVENT_PRESENCE:
    {
        const radar_presence_data_t* data = (const radar_presence_data_t*)event->data;
        if (data != NULL)
        {
            lvgl_radar_view_set_data(0.0f, 0.0f, data->is_present);
            lvgl_radar_view_update();
        }
        break;
    }
    case RADAR_EVENT_BREATH:
    {
        const radar_breath_data_t* data = (const radar_breath_data_t*)event->data;
        if (data != NULL && data->is_detected)
        {
            lvgl_radar_view_add_breath_point(data->breath_rate);
            lvgl_radar_view_update();
        }
        break;
    }
    case RADAR_EVENT_HEART_RATE:
    {
        const radar_heart_data_t* data = (const radar_heart_data_t*)event->data;
        if (data != NULL && data->is_valid)
        {
            lvgl_radar_view_add_heart_point(data->heart_rate);
            lvgl_radar_view_update();
        }
        break;
    }
#endif
    default:
        break;
    }

    xSemaphoreGive(s_lvgl_mutex);
}

esp_err_t lvgl_ui_init(void)
{
    if (s_lvgl_ui.initialized)
    {
        ESP_LOGW(TAG, "LVGL UI already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LVGL UI");

    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = lcd_driver_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LCD driver: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_lvgl_mutex);
        s_lvgl_mutex = NULL;
        return ret;
    }

    if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE)
    {
        lv_init();
        lv_tick_set_cb(lvgl_tick_get_cb);

        ret = lvgl_disp_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize LVGL display: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_lvgl_mutex);
            vSemaphoreDelete(s_lvgl_mutex);
            s_lvgl_mutex = NULL;
            return ret;
        }

#if (LVGL_UI_ENABLE_ENCODER == 1)
        lv_group_t* group = lv_group_create();
        lv_group_set_default(group);
#endif

        s_lvgl_ui.main_screen = lv_scr_act();
        if (s_lvgl_ui.main_screen == NULL)
        {
            ESP_LOGE(TAG, "Failed to get active screen");
            xSemaphoreGive(s_lvgl_mutex);
            vSemaphoreDelete(s_lvgl_mutex);
            s_lvgl_mutex = NULL;
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Using active screen: %p", s_lvgl_ui.main_screen);

#if (LVGL_UI_ENABLE_SYSMON_PERF == 1)
        lv_sysmon_show_performance(lvgl_disp_get());
        ESP_LOGI(TAG, "LVGL performance monitor enabled");
#endif

#if (LVGL_UI_ENABLE_SYSMON_MEM == 1)
        lv_sysmon_show_memory(lvgl_disp_get());
        ESP_LOGI(TAG, "LVGL memory monitor enabled");
#endif

        xSemaphoreGive(s_lvgl_mutex);
    }

    s_lvgl_ui.initialized    = true;
    s_lvgl_ui.event_callback = NULL;

    ret = event_bus_subscribe(EVENT_TYPE_RADAR_DATA, radar_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to subscribe to radar events: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Subscribed to radar events");
    }

    load_page(s_lvgl_ui.current_page);

    ESP_LOGI(TAG, "LVGL UI initialized successfully");
    return ESP_OK;
}

esp_err_t lvgl_ui_deinit(void)
{
    if (!s_lvgl_ui.initialized)
    {
        ESP_LOGW(TAG, "LVGL UI not initialized");
        return ESP_OK;
    }

    event_bus_unsubscribe(EVENT_TYPE_RADAR_DATA, radar_event_handler);

    if (s_lvgl_mutex != NULL)
    {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
            lvgl_expression_deinit();
#endif
#if (LVGL_UI_ENABLE_INFO == 1)
            lvgl_info_deinit();
#endif
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
            lvgl_radar_view_deinit();
#endif

            if (s_lvgl_ui.main_screen != NULL)
            {
                lv_obj_delete(s_lvgl_ui.main_screen);
                s_lvgl_ui.main_screen = NULL;
            }

#if (LVGL_UI_ENABLE_ENCODER == 1)
            if (s_encoder_indev != NULL)
            {
                lv_indev_delete(s_encoder_indev);
                s_encoder_indev = NULL;
            }
#endif

            lvgl_disp_deinit();

            xSemaphoreGive(s_lvgl_mutex);
        }
    }

    lcd_driver_deinit();

    if (s_lvgl_mutex != NULL)
    {
        vSemaphoreDelete(s_lvgl_mutex);
        s_lvgl_mutex = NULL;
    }

    s_lvgl_ui.initialized    = false;
    s_lvgl_ui.event_callback = NULL;

    ESP_LOGI(TAG, "LVGL UI deinitialized");
    return ESP_OK;
}

void lvgl_ui_update(void)
{
    if (!s_lvgl_ui.initialized)
    {
        return;
    }

    if (s_lvgl_mutex == NULL)
    {
        return;
    }

    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to take LVGL mutex in update (timeout 100ms)");
        ESP_LOGW(TAG, "Page switch operation may be in progress");
        return;
    }

#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    if (s_lvgl_ui.current_page == LVGL_UI_PAGE_EXPRESSION)
    {
        lvgl_expression_update();
        xSemaphoreGive(s_lvgl_mutex);
        return;
    }
#endif

    lv_timer_handler();

    switch (s_lvgl_ui.current_page)
    {
#if (LVGL_UI_ENABLE_INFO == 1)
    case LVGL_UI_PAGE_INFO:
        lvgl_info_update();
        break;
#endif
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    case LVGL_UI_PAGE_RADAR_VIEW:
        lvgl_radar_view_update();
        break;
#endif
    default:
        break;
    }

    xSemaphoreGive(s_lvgl_mutex);
}

esp_err_t lvgl_ui_switch_page(lvgl_ui_page_t page)
{
    if (!s_lvgl_ui.initialized)
    {
        ESP_LOGW(TAG, "LVGL UI not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (page < 0 || page >= LVGL_UI_PAGE_COUNT)
    {
        ESP_LOGW(TAG, "Invalid page: %d", page);
        return ESP_ERR_INVALID_ARG;
    }

    load_page(page);
    ESP_LOGI(TAG, "Switched to page %d", page);
    return ESP_OK;
}

esp_err_t lvgl_ui_next_page(void)
{
    lvgl_ui_page_t next = (s_lvgl_ui.current_page + 1) % LVGL_UI_PAGE_COUNT;
    return lvgl_ui_switch_page(next);
}

lvgl_ui_page_t lvgl_ui_get_current_page(void)
{
    return s_lvgl_ui.current_page;
}

esp_err_t lvgl_ui_set_expression(lvgl_ui_expression_t expr)
{
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    return lvgl_expression_set(expr);
#else
    (void)expr;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

lvgl_ui_expression_t lvgl_ui_get_expression(void)
{
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    return lvgl_expression_get();
#else
    return LVGL_UI_EXPR_NORMAL;
#endif
}

void lvgl_ui_set_auto_blink(bool enable)
{
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    lvgl_expression_set_auto_blink(enable);
#else
    (void)enable;
#endif
}

bool lvgl_ui_is_auto_blink(void)
{
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    return lvgl_expression_is_auto_blink();
#else
    return false;
#endif
}

esp_err_t lvgl_ui_set_info_title(const char* title)
{
#if (LVGL_UI_ENABLE_INFO == 1)
    return lvgl_info_set_title(title);
#else
    (void)title;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t lvgl_ui_set_info_value(int value)
{
#if (LVGL_UI_ENABLE_INFO == 1)
    return lvgl_info_set_value(value);
#else
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t lvgl_ui_set_info_progress(int progress)
{
#if (LVGL_UI_ENABLE_INFO == 1)
    return lvgl_info_set_progress(progress);
#else
    (void)progress;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t lvgl_ui_set_info_status(bool status)
{
#if (LVGL_UI_ENABLE_INFO == 1)
    return lvgl_info_set_status(status);
#else
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t lvgl_ui_set_info_timer(uint32_t seconds)
{
#if (LVGL_UI_ENABLE_INFO == 1)
    return lvgl_info_set_time((int)seconds);
#else
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t lvgl_ui_register_callback(lvgl_ui_event_callback_t callback)
{
    if (!s_lvgl_ui.initialized)
    {
        ESP_LOGW(TAG, "LVGL UI not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_lvgl_ui.event_callback = callback;
    return ESP_OK;
}

void lvgl_ui_send_event(const lvgl_ui_event_t* event)
{
    if (!s_lvgl_ui.initialized || event == NULL)
    {
        return;
    }

    switch (s_lvgl_ui.current_page)
    {
#if (LVGL_UI_ENABLE_EXPRESSION == 1)
    case LVGL_UI_PAGE_EXPRESSION:
        switch (event->type)
        {
        case LVGL_UI_EVENT_HUMAN_PRESENCE:
            lvgl_expression_set(event->data.is_present ? LVGL_UI_EXPR_NORMAL : LVGL_UI_EXPR_SLEEPY);
            break;
        case LVGL_UI_EVENT_MOTION:
            lvgl_expression_set(LVGL_UI_EXPR_SURPRISED);
            break;
        default:
            break;
        }
        break;
#endif

#if (LVGL_UI_ENABLE_INFO == 1)
    case LVGL_UI_PAGE_INFO:
        switch (event->type)
        {
        case LVGL_UI_EVENT_HUMAN_PRESENCE:
            lvgl_info_set_title(event->data.is_present ? "Detecting" : "Idle");
            lvgl_info_set_status(event->data.is_present);
            break;
        case LVGL_UI_EVENT_BREATH_RATE:
            lvgl_info_set_title("Breath");
            lvgl_info_set_value((int)event->data.breath_rate);
            break;
        case LVGL_UI_EVENT_HEART_RATE:
            lvgl_info_set_title("Heart");
            lvgl_info_set_value((int)event->data.heart_rate);
            break;
        default:
            break;
        }
        break;
#endif

#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    case LVGL_UI_PAGE_RADAR_VIEW:
        switch (event->type)
        {
        case LVGL_UI_EVENT_BREATH_RATE:
            lvgl_radar_view_add_breath_point(event->data.breath_rate);
            break;
        case LVGL_UI_EVENT_HEART_RATE:
            lvgl_radar_view_add_heart_point(event->data.heart_rate);
            break;
        default:
            break;
        }
        break;
#endif

    default:
        break;
    }

    if (s_lvgl_ui.event_callback != NULL)
    {
        s_lvgl_ui.event_callback(event);
    }
}

void lvgl_ui_set_radar_data(float breath_rate, float heart_rate, bool is_present)
{
#if (LVGL_UI_ENABLE_RADAR_VIEW == 1)
    lvgl_radar_view_set_data(breath_rate, heart_rate, is_present);
#else
    (void)breath_rate;
    (void)heart_rate;
    (void)is_present;
#endif
}

#endif
