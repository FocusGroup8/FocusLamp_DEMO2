# Audio storage - requires esp_littlefs from IDF component manager
# Install with: idf.py add-dependency "esp_littlefs"
# Temporarily disabled - no source compiled until esp_littlefs is available
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES common
)
