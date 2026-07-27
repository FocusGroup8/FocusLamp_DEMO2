/**
 * @file param_registry.h
 * @brief Runtime Parameter Registry for ESP-IDF Console
 *
 * Provides a centralized registry for runtime-adjustable parameters.
 * Parameters are registered with metadata (name, category, type, range)
 * and can be queried/modified via console commands (set/get/list).
 *
 * Usage:
 *   1. Call param_registry_init() at startup
 *   2. Register parameters with param_register_xxx() functions
 *   3. Use param_get()/param_set() to access values
 *   4. Console commands (set/get/list/reset) operate via registry
 *
 * Phase 1: Core framework + informational access
 * Phase 2: NVS persistence + full wiring to modules
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

/** Maximum number of registrable parameters */
#define PARAM_REGISTRY_MAX_PARAMS 64

/** Maximum length of parameter name */
#define PARAM_NAME_MAX_LEN 32

/** Maximum length of category name */
#define PARAM_CATEGORY_MAX_LEN 16

/** Maximum length of description text */
#define PARAM_DESC_MAX_LEN 64

/** Maximum length of unit string */
#define PARAM_UNIT_MAX_LEN 8

/* ============================================================
 * Parameter Types
 * ============================================================ */

typedef enum
{
    PARAM_TYPE_INT = 0,    /**< Integer value */
    PARAM_TYPE_FLOAT,      /**< Floating-point value */
    PARAM_TYPE_BOOL,       /**< Boolean (0/1) */
    PARAM_TYPE_ENUM,       /**< Enumerated (int with string labels) */
    PARAM_TYPE_READ_ONLY,  /**< Informational (cannot be set) */
} param_type_t;

/* ============================================================
 * Parameter Entry Structure
 * ============================================================ */

/**
 * @brief Runtime parameter descriptor
 *
 * Each registered parameter has:
 * - Metadata: name, category, description, type, range
 * - Storage: pointer to the actual variable (or getter/setter callbacks)
 * - Default: original value for reset functionality
 */
typedef struct
{
    /* Metadata */
    char name[PARAM_NAME_MAX_LEN];         /**< Parameter identifier */
    char category[PARAM_CATEGORY_MAX_LEN]; /**< Grouping category */
    char description[PARAM_DESC_MAX_LEN];  /**< Human-readable description */
    char unit[PARAM_UNIT_MAX_LEN];        /**< Unit string (e.g., "cm", "ms") */
    param_type_t type;                     /**< Data type */

    /* Range constraints */
    float min_val;                         /**< Minimum allowed value */
    float max_val;                         /**< Maximum allowed value */

    /* Value access */
    void*        ptr;                      /**< Pointer to runtime variable */
    float        default_val;              /**< Default value for reset */

    /* Optional callbacks (if ptr is NULL, use callbacks) */
    float (*getter)(void);                 /**< Custom getter function */
    void  (*setter)(float value);          /**< Custom setter function */

    /* State */
    bool registered;                       /**< Whether this slot is in use */
} runtime_param_t;

/* ============================================================
 * Registry API
 * ============================================================ */

/**
 * @brief Initialize the parameter registry
 * @return true on success, false on failure
 */
bool param_registry_init(void);

/**
 * @brief Get number of registered parameters
 * @return Count of registered parameters
 */
size_t param_registry_count(void);

/**
 * @brief Register a parameter from a pre-filled descriptor
 * @param param  Pre-filled parameter descriptor
 * @return true on success, false if registry full or invalid
 */
bool param_register(const runtime_param_t* param);

/**
 * @brief Find a parameter by name
 * @param name  Parameter name (case-sensitive)
 * @return Pointer to parameter entry, or NULL if not found
 */
runtime_param_t* param_find(const char* name);

/**
 * @brief Get current value of a parameter as float
 * @param name  Parameter name
 * @param out_value  Output: current value
 * @return true if found, false if not found
 */
bool param_get_float(const char* name, float* out_value);

/**
 * @brief Set value of a parameter
 * @param name   Parameter name
 * @param value  New value
 * @return true on success, false if not found/out of range/read-only
 */
bool param_set_float(const char* name, float value);

/**
 * @brief Reset a parameter to its default value
 * @param name  Parameter name (or NULL for all)
 * @return Number of parameters reset
 */
int param_reset(const char* name);

/**
 * @brief Reset all parameters to defaults
 * @return Number of parameters reset
 */
int param_reset_all(void);

/**
 * @brief Iterate over parameters in a category
 * @param category  Category name (or NULL for all)
 * @param callback  Called for each matching parameter
 * @param user_data  Passed to callback
 * @return Number of parameters iterated
 */
typedef void (*param_iter_callback_t)(const runtime_param_t* param, void* user_data);
size_t param_iterate_category(const char* category, param_iter_callback_t callback,
                              void* user_data);

/**
 * @brief Get list of all categories
 * @param out_categories  Output array of category strings
 * @param max_categories  Size of output array
 * @return Number of categories found
 */
size_t param_list_categories(char (*out_categories)[PARAM_CATEGORY_MAX_LEN], size_t max_categories);

/* ============================================================
 * Typed Registration Functions (preferred over raw param_register)
 *
 * These avoid compound literal issues with some C compilers.
 * ============================================================ */

/**
 * @brief Register an integer parameter
 * @param name        Parameter identifier
 * @param category    Grouping category
 * @param description Human-readable description
 * @param unit        Unit string (e.g., "cm", "ms", "")
 * @param min_val     Minimum allowed value
 * @param max_val     Maximum allowed value
 * @param var_ptr     Pointer to the int variable
 * @return true on success
 */
bool param_register_int(const char* name, const char* category, const char* description,
                        const char* unit, int min_val, int max_val, int* var_ptr);

/**
 * @brief Register a float parameter
 * @param name        Parameter identifier
 * @param category    Grouping category
 * @param description Human-readable description
 * @param unit        Unit string (e.g., "cm", "ms", "")
 * @param min_val     Minimum allowed value
 * @param max_val     Maximum allowed value
 * @param var_ptr     Pointer to the float variable
 * @return true on success
 */
bool param_register_float(const char* name, const char* category, const char* description,
                          const char* unit, float min_val, float max_val, float* var_ptr);

/**
 * @brief Register a boolean parameter
 * @param name        Parameter identifier
 * @param category    Grouping category
 * @param description Human-readable description
 * @param var_ptr     Pointer to the bool variable
 * @return true on success
 */
bool param_register_bool(const char* name, const char* category, const char* description,
                         bool* var_ptr);

/**
 * @brief Register a read-only (informational) parameter
 * @param name        Parameter identifier
 * @param category    Grouping category
 * @param description Human-readable description
 * @param unit        Unit string
 * @param getter_fn   Getter function returning current value
 * @return true on success
 */
bool param_register_ro(const char* name, const char* category, const char* description,
                       const char* unit, float (*getter_fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* PARAM_REGISTRY_H */
