/**
 * @file param_registry.h
 * @brief Runtime Parameter Registry for ESP-IDF Console
 *
 * Provides a centralized registry for runtime-adjustable parameters.
 * Parameters are registered with metadata (name, category, type, range)
 * and can be queried/modified via console commands (set/get/list).
 */

#ifndef PARAM_REGISTRY_H
#define PARAM_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================================================
 * Constants
 * ============================================================ */

#define PARAM_REGISTRY_MAX_PARAMS 64
#define PARAM_NAME_MAX_LEN 32
#define PARAM_CATEGORY_MAX_LEN 16
#define PARAM_DESC_MAX_LEN 64
#define PARAM_UNIT_MAX_LEN 8

/* ============================================================
 * Parameter Types
 * ============================================================ */

typedef enum
{
    PARAM_TYPE_INT = 0,
    PARAM_TYPE_FLOAT,
    PARAM_TYPE_BOOL,
    PARAM_TYPE_ENUM,
    PARAM_TYPE_READ_ONLY,
} param_type_t;

/* ============================================================
 * Parameter Entry Structure
 * ============================================================ */

typedef struct
{
    char name[PARAM_NAME_MAX_LEN];
    char category[PARAM_CATEGORY_MAX_LEN];
    char description[PARAM_DESC_MAX_LEN];
    char unit[PARAM_UNIT_MAX_LEN];
    param_type_t type;

    float min_val;
    float max_val;

    void* ptr;
    float default_val;

    float (*getter)(void);
    void  (*setter)(float value);

    bool registered;
} runtime_param_t;

/* ============================================================
 * Registry API
 * ============================================================ */

bool param_registry_init(void);
size_t param_registry_count(void);
bool param_register(const runtime_param_t* param);
runtime_param_t* param_find(const char* name);
bool param_get_float(const char* name, float* out_value);
bool param_set_float(const char* name, float value);
int param_reset(const char* name);
int param_reset_all(void);

typedef void (*param_iter_callback_t)(const runtime_param_t* param, void* user_data);
size_t param_iterate_category(const char* category, param_iter_callback_t callback,
                              void* user_data);
size_t param_list_categories(char (*out_categories)[PARAM_CATEGORY_MAX_LEN], size_t max_categories);

bool param_register_int(const char* name, const char* category, const char* description,
                        const char* unit, int min_val, int max_val, int* var_ptr);
bool param_register_float(const char* name, const char* category, const char* description,
                          const char* unit, float min_val, float max_val, float* var_ptr);
bool param_register_bool(const char* name, const char* category, const char* description,
                         bool* var_ptr);
bool param_register_ro(const char* name, const char* category, const char* description,
                       const char* unit, float (*getter_fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* PARAM_REGISTRY_H */
