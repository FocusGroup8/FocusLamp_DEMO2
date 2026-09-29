"""
面部关键点检测器抽象基类

继承 MediaPipeAsyncProvider，复用 LIVE_STREAM 异步模板代码（run/close/save_result）。
子类只需实现 _extract_result 和 get_detect_result。
"""

from detectors.base.mediapipe_async_provider import MediaPipeAsyncProvider

TAG = __name__


class FaceLandmarkerProviderBase(MediaPipeAsyncProvider):
    """面部关键点检测器抽象基类"""
    pass
