# Audio processor - requires esp_afe_sr_models from ESP-SR
# Install with: idf.py add-dependency "esp_afe_sr_models"
# Temporarily disabled - no source compiled until esp_afe_sr_models is available
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES common
)
