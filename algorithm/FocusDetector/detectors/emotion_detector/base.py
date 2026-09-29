import numpy as np
from abc import ABC, abstractmethod

from utils.logger import setup_logging

TAG = __name__


class EmotionDetectorProviderBase(ABC):
    """情绪检测器抽象基类。

    统一 Detector 接口：
      - detect(image): 同步检测（emotion 是同步 API，区别于 MediaPipe 异步）
      - close(): 释放资源（emotion 无资源，默认空实现）
      - __enter__/__exit__: 支持 with 语法
    """

    def __init__(self):
        super().__init__()
        self.logger = setup_logging()

    @abstractmethod
    def detect(self, image: np.ndarray) -> dict | None:
        pass

    def close(self):
        """释放资源。emotion detector 无外部资源，默认空实现。"""
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False
