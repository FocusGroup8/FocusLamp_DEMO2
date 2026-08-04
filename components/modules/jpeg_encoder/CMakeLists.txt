# JPEG encoder - requires esp_video from ESP-IDF component manager
# Install with: idf.py add-dependency "esp_video"
# Temporarily disabled - no source compiled until esp_video is available
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES common
)
