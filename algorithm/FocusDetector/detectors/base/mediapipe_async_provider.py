"""
MediaPipe LIVE_STREAM 异步 Provider 通用基类

抽取 GestureDetector / PoseDetector / FaceLandmarker 三个 Provider 的公共代码：
  - LIVE_STREAM 异步模式 + result_callback 回调模式
  - run(image) 通用提交逻辑（mp.Image + detect_async + timestamp）
  - close() 通用释放逻辑
  - save_result 回调模板（统一异常处理 + 调用子类 _extract_result）

子类职责：
  - 在 __init__ 中调用 super().__init__()，然后构造自己的 options 和 self._landmarker
  - 实现 _extract_result(result)：从 MediaPipe result 中提取字段存到子类属性
  - 实现 get_detect_result() -> dict | None：返回统一 dict 格式

回调签名（MediaPipe LIVE_STREAM 约定）：
  def callback(result, output_image: mp.Image, timestamp_ms: int): ...
"""

import time
import numpy as np
import mediapipe as mp

from abc import ABC, abstractmethod

from utils.logger import setup_logging

TAG = __name__


class MediaPipeAsyncProvider(ABC):
    """MediaPipe LIVE_STREAM 异步检测器的通用基类。"""

    def __init__(self):
        self.logger = setup_logging()
        self.recognition_result = None     # 最近的原始 result（供子类调试/扩展用）
        self._landmarker = None             # 子类在 __init__ 中创建具体的 landmarker 对象

    # ===== 通用方法（子类无需重写）=====

    def _make_callback(self):
        """构造 MediaPipe LIVE_STREAM 的 result_callback。

        统一处理异常并调用子类的 _extract_result 提取字段。
        """
        def save_result(result, unused_output_image: mp.Image, timestamp_ms: int):
            try:
                self.recognition_result = result
                self._extract_result(result)
            except Exception as e:
                self.logger.bind(tag=TAG).error(f'Failed to save result in {self.__class__.__name__}: {e}')
        return save_result

    def run(self, image: np.ndarray):
        """异步提交一帧图像给 MediaPipe。

        通用流程：构造 mp.Image + 时间戳 + try/except + 调用子类 _submit_async。
        子类通过 _submit_async 指定具体的异步方法名（detect_async / recognize_async）。

        Args:
            image: RGB 图像 (H, W, 3) numpy 数组
        """
        try:
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
            self._submit_async(mp_image, time.time_ns() // 1_000_000)
        except Exception as e:
            self.logger.bind(tag=TAG).error(f'Failed to run {self.__class__.__name__}: {e}')

    def _submit_async(self, mp_image: mp.Image, timestamp_ms: int):
        """提交一帧给 MediaPipe landmarker 的异步方法。

        子类必须重写此方法，调用具体的异步 API：
          - GestureRecognizer: self._landmarker.recognize_async(...)
          - PoseLandmarker:   self._landmarker.detect_async(...)
          - FaceLandmarker:   self._landmarker.detect_async(...)
        """
        raise NotImplementedError(f'{self.__class__.__name__} must implement _submit_async')

    def close(self):
        """释放 MediaPipe landmarker 资源。"""
        try:
            if self._landmarker is not None:
                self._landmarker.close()
        except Exception as e:
            self.logger.bind(tag=TAG).error(f'{self.__class__.__name__} close failed: {e}')

    def __enter__(self):
        """支持 with 语法，资源自动释放"""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    # ===== 抽象方法（子类必须实现）=====

    @abstractmethod
    def _extract_result(self, result) -> None:
        """从 MediaPipe result 中提取字段，存储到子类自身属性。

        由 _make_callback 在收到结果时调用。子类应在此方法中：
          1. 重置自身字段为 None（避免返回过期数据）
          2. 检查 result 字段，若有效则更新自身字段
        """
        pass

    @abstractmethod
    def get_detect_result(self) -> dict | None:
        """返回最近一次有效检测结果。

        Returns:
            dict | None: 无结果时返回 None；有结果时返回固定格式的 dict（子类决定字段）
        """
        pass
