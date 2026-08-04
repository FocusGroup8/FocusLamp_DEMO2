# Voice control module - requires esp-sr (esp_afe_sr_models, esp_mn_models, etc.)
# from IDF component manager or ESP Registry.
# Install with: idf.py add-dependency "esp_afe_sr_models^2.0.0"
# Temporarily disabled - no source compiled until esp-sr dependencies are available
idf_component_register(
    INCLUDE_DIRS 
        "include"
        "include/mcp_tools"
    REQUIRES
        common
        event_bus
        drivers
)
