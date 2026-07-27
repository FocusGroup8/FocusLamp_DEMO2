/**
 * @file param_registry.c
 * @brief Runtime Parameter Registry Implementation
 *
 * Implements the parameter registry for runtime-adjustable configuration.
 * Supports int/float/bool/read-only parameter types with range checking.
 */

#include "param_registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "param_registry";

/* ============================================================
 * Internal Storage
 * ============================================================ */

static runtime_param_t s_param_table[PARAM_REGISTRY_MAX_PARAMS];
static size_t          s_param_count  = 0;
static bool             s_initialized = false;

/* ============================================================
 * Helper: Read value from a parameter entry
 * ============================================================ */

static float param_read_value(const runtime_param_t* param)
{
    if (param->getter != NULL)
    {
        return param->getter();
    }
    if (param->ptr != NULL)
    {
        switch (param->type)
        {
            case PARAM_TYPE_INT:
            case PARAM_TYPE_BOOL:
            case PARAM_TYPE_ENUM:
                return (float)(*(int*)param->ptr);
            case PARAM_TYPE_FLOAT:
            case PARAM_TYPE_READ_ONLY:
                return *(float*)param->ptr;
            default:
                return 0.0f;
        }
    }
    return 0.0f;
}

/* ============================================================
 * Helper: Write value to a parameter entry
 * ============================================================ */

static bool param_write_value(runtime_param_t* param, float value)
{
    /* Read-only check */
    if (param->type == PARAM_TYPE_READ_ONLY)
    {
        ESP_LOGW(TAG, "Parameter '%s' is read-only", param->name);
        return false;
    }

    /* Range check */
    if (value < param->min_val || value > param->max_val)
    {
        ESP_LOGW(TAG, "Value %.4f out of range [%.4f, %.4f] for '%s'", value, param->min_val,
                 param->max_val, param->name);
        return false;
    }

    /* Write */
    if (param->setter != NULL)
    {
        param->setter(value);
        return true;
    }
    if (param->ptr != NULL)
    {
        switch (param->type)
        {
            case PARAM_TYPE_INT:
                *(int*)param->ptr = (int)value;
                break;
            case PARAM_TYPE_BOOL:
                *(bool*)param->ptr = (value >= 0.5f);
                break;
            case PARAM_TYPE_FLOAT:
                *(float*)param->ptr = value;
                break;
            case PARAM_TYPE_ENUM:
                *(int*)param->ptr = (int)value;
                break;
            default:
                return false;
        }
        ESP_LOGD(TAG, "'%s' set to %g %s", param->name, value, param->unit);
        return true;
    }
    return false;
}

/* ============================================================
 * Public API Implementation
 * ============================================================ */

bool param_registry_init(void)
{
    memset(s_param_table, 0, sizeof(s_param_table));
    s_param_count  = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (capacity: %d)", PARAM_REGISTRY_MAX_PARAMS);
    return true;
}

size_t param_registry_count(void)
{
    return s_param_count;
}

bool param_register(const runtime_param_t* param)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Registry not initialized");
        return false;
    }
    if (param == NULL || param->name[0] == '\0')
    {
        ESP_LOGE(TAG, "Invalid parameter (null or empty name)");
        return false;
    }
    if (s_param_count >= PARAM_REGISTRY_MAX_PARAMS)
    {
        ESP_LOGE(TAG, "Registry full (%d max)", PARAM_REGISTRY_MAX_PARAMS);
        return false;
    }

    /* Check for duplicate name */
    for (size_t i = 0; i < s_param_count; i++)
    {
        if (strcmp(s_param_table[i].name, param->name) == 0)
        {
            ESP_LOGW(TAG, "Duplicate parameter '%s', skipping", param->name);
            return false;
        }
    }

    memcpy(&s_param_table[s_param_count], param, sizeof(runtime_param_t));
    s_param_table[s_param_count].registered = true;
    s_param_count++;

    ESP_LOGD(TAG, "Registered [%s] %s (type=%d, default=%g)", param->category, param->name,
             param->type, param->default_val);
    return true;
}

runtime_param_t* param_find(const char* name)
{
    if (name == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < s_param_count; i++)
    {
        if (strcmp(s_param_table[i].name, name) == 0)
        {
            return &s_param_table[i];
        }
    }
    return NULL;
}

bool param_get_float(const char* name, float* out_value)
{
    const runtime_param_t* p = param_find(name);
    if (p == NULL || out_value == NULL)
    {
        return false;
    }
    *out_value = param_read_value(p);
    return true;
}

bool param_set_float(const char* name, float value)
{
    runtime_param_t* p = param_find(name);
    if (p == NULL)
    {
        ESP_LOGW(TAG, "Parameter '%s' not found", name);
        return false;
    }
    return param_write_value(p, value);
}

int param_reset(const char* name)
{
    if (name == NULL || strcmp(name, "all") == 0)
    {
        return param_reset_all();
    }

    runtime_param_t* p = param_find(name);
    if (p == NULL)
    {
        ESP_LOGW(TAG, "Parameter '%s' not found for reset", name);
        return 0;
    }

    float old_val = param_read_value(p);
    param_write_value(p, p->default_val);
    ESP_LOGI(TAG, "'%s' reset: %g -> %g %s", name, old_val, p->default_val, p->unit);
    return 1;
}

int param_reset_all(void)
{
    int count = 0;
    for (size_t i = 0; i < s_param_count; i++)
    {
        if (s_param_table[i].type != PARAM_TYPE_READ_ONLY)
        {
            param_write_value(&s_param_table[i], s_param_table[i].default_val);
            count++;
        }
    }
    ESP_LOGI(TAG, "Reset %d/%d parameters to defaults", count, (int)s_param_count);
    return count;
}

size_t param_iterate_category(const char* category, param_iter_callback_t callback,
                              void* user_data)
{
    size_t count = 0;
    for (size_t i = 0; i < s_param_count; i++)
    {
        if (category == NULL || strcmp(s_param_table[i].category, category) == 0)
        {
            if (callback != NULL)
            {
                callback(&s_param_table[i], user_data);
            }
            count++;
        }
    }
    return count;
}

size_t param_list_categories(char (*out_categories)[PARAM_CATEGORY_MAX_LEN],
                             size_t max_categories)
{
    size_t cat_count = 0;

    for (size_t i = 0; i < s_param_count && cat_count < max_categories; i++)
    {
        bool duplicate = false;
        for (size_t j = 0; j < cat_count; j++)
        {
            if (strcmp(out_categories[j], s_param_table[i].category) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            strncpy(out_categories[cat_count], s_param_table[i].category,
                    PARAM_CATEGORY_MAX_LEN - 1);
            out_categories[cat_count][PARAM_CATEGORY_MAX_LEN - 1] = '\0';
            cat_count++;
        }
    }
    return cat_count;
}

/* ============================================================
 * Typed Registration Helpers
 * ============================================================ */

bool param_register_int(const char* name, const char* category, const char* description,
                        const char* unit, int min_val, int max_val, int* var_ptr)
{
    runtime_param_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name ? name : "", PARAM_NAME_MAX_LEN - 1);
    strncpy(p.category, category ? category : "", PARAM_CATEGORY_MAX_LEN - 1);
    strncpy(p.description, description ? description : "", PARAM_DESC_MAX_LEN - 1);
    strncpy(p.unit, unit ? unit : "", PARAM_UNIT_MAX_LEN - 1);
    p.type        = PARAM_TYPE_INT;
    p.min_val     = (float)min_val;
    p.max_val     = (float)max_val;
    p.ptr         = var_ptr;
    p.default_val = var_ptr ? (float)(*var_ptr) : 0.0f;
    p.getter      = NULL;
    p.setter      = NULL;
    p.registered  = false;
    return param_register(&p);
}

bool param_register_float(const char* name, const char* category, const char* description,
                          const char* unit, float min_val, float max_val, float* var_ptr)
{
    runtime_param_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name ? name : "", PARAM_NAME_MAX_LEN - 1);
    strncpy(p.category, category ? category : "", PARAM_CATEGORY_MAX_LEN - 1);
    strncpy(p.description, description ? description : "", PARAM_DESC_MAX_LEN - 1);
    strncpy(p.unit, unit ? unit : "", PARAM_UNIT_MAX_LEN - 1);
    p.type        = PARAM_TYPE_FLOAT;
    p.min_val     = min_val;
    p.max_val     = max_val;
    p.ptr         = var_ptr;
    p.default_val = var_ptr ? (*var_ptr) : 0.0f;
    p.getter      = NULL;
    p.setter      = NULL;
    p.registered  = false;
    return param_register(&p);
}

bool param_register_bool(const char* name, const char* category, const char* description,
                         bool* var_ptr)
{
    runtime_param_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name ? name : "", PARAM_NAME_MAX_LEN - 1);
    strncpy(p.category, category ? category : "", PARAM_CATEGORY_MAX_LEN - 1);
    strncpy(p.description, description ? description : "", PARAM_DESC_MAX_LEN - 1);
    p.unit[0]     = '\0';
    p.type        = PARAM_TYPE_BOOL;
    p.min_val     = 0.0f;
    p.max_val     = 1.0f;
    p.ptr         = var_ptr;
    p.default_val = var_ptr ? ((*var_ptr) ? 1.0f : 0.0f) : 0.0f;
    p.getter      = NULL;
    p.setter      = NULL;
    p.registered  = false;
    return param_register(&p);
}

bool param_register_ro(const char* name, const char* category, const char* description,
                       const char* unit, float (*getter_fn)(void))
{
    runtime_param_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name ? name : "", PARAM_NAME_MAX_LEN - 1);
    strncpy(p.category, category ? category : "", PARAM_CATEGORY_MAX_LEN - 1);
    strncpy(p.description, description ? description : "", PARAM_DESC_MAX_LEN - 1);
    strncpy(p.unit, unit ? unit : "", PARAM_UNIT_MAX_LEN - 1);
    p.type        = PARAM_TYPE_READ_ONLY;
    p.min_val     = 0.0f;
    p.max_val     = 0.0f;
    p.ptr         = NULL;
    p.default_val = 0.0f;
    p.getter      = getter_fn;
    p.setter      = NULL;
    p.registered  = false;
    return param_register(&p);
}
