# Console Utilities Component
#
# Provides unified macros, helpers, and runtime parameter registry
# for ESP-IDF console command development.

idf_component_register(
    SRCS "src/param_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES console
)
