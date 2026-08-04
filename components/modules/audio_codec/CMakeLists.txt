# Audio OGG Opus codec - requires micro_opus from IDF component manager
# Install with: idf.py add-dependency "micro_opus"
# Temporarily disabled - no source files compiled until micro_opus is available
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES common
)
