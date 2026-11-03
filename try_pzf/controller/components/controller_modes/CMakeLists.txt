idf_component_register(
    SRCS "src/controller_modes.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_timer freertos esp_hw_support
)

option(FOCUS_SAMPLED_DEMO "Enable sampled countdown demonstration settings" OFF)
if(FOCUS_SAMPLED_DEMO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOCUS_SAMPLED_DEMO=1)
endif()
