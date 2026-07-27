# Smart voice control module - requires esp_xiaozhi, opus, esp-sr, esp_mcp
# from IDF component manager or ESP Registry.
# Install with: 
#   idf.py add-dependency "esp_xiaozhi^1.0.0" "opus^1.4.0" "esp_afe_sr_models^2.0.0"
# Temporarily disabled - no source compiled until dependencies are available
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES
        "common"
        "event_bus"
        "wifi_manager"
        "drivers"
        # "audio_storage" /* TODO: enable when esp_littlefs is available */
        "json"
        "console"
    PRIV_REQUIRES
        "esp_timer"
        "esp_wifi"
        "nvs_flash"
)
