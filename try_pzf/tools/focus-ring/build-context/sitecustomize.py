"""Only enabled by build.ps1 for the locked head dependency resolver.

esp_video 2.3.0 pins esp_h264 1.3.0. A newer registry candidate references
an option absent from that version. Supply its disabled default to the
resolver context, without editing sdkconfig, SDK files or installed packages.
"""
import os
if os.environ.get('FOCUS_HEAD_LOCKED_BUILD') == '1':
    from idf_component_tools.debugger import KCONFIG_CONTEXT
    KCONFIG_CONTEXT.get().sdkconfig.setdefault('ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION', False)
