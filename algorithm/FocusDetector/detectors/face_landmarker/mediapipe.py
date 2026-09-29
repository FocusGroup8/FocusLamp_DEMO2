"""
MediaPipe FaceLandmarker 面部关键点检测器

使用 LIVE_STREAM 异步模式，检测面部的 478 个关键点（468 个面部 + 10 个虹膜）。

输出接口：
  run(image)                异步提交一帧
  get_detect_result() -> dict | None
    {
      'face_landmarks': List[NormalizedLandmark],  # 第一张脸的 478 个关键点
    }
"""

import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from detectors.face_landmarker.base import FaceLandmarkerProviderBase

TAG = __name__


class FaceLandmarkerProvider(FaceLandmarkerProviderBase):
    def __init__(
        self,
        model_path: str = "checkpoint/face_landmarker.task",
        num_faces: int = 1,
        min_face_detection_confidence: float = 0.5,
        min_face_presence_confidence: float = 0.5,
        min_tracking_confidence: float = 0.5,
    ):
        super().__init__()
        try:
            self.face_landmarks = None

            base_options = python.BaseOptions(model_asset_path=model_path)
            options = vision.FaceLandmarkerOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                num_faces=num_faces,
                min_face_detection_confidence=min_face_detection_confidence,
                min_face_presence_confidence=min_face_presence_confidence,
                min_tracking_confidence=min_tracking_confidence,
                result_callback=self._make_callback(),
            )
            self._landmarker = vision.FaceLandmarker.create_from_options(options)
        except Exception as e:
            self.logger.bind(tag=TAG).error(f'Failed to initialize FaceLandmarkerProvider: {e}')
            raise e

    def _submit_async(self, mp_image, timestamp_ms: int):
        self._landmarker.detect_async(mp_image, timestamp_ms)

    def _extract_result(self, result) -> None:
        """从 FaceLandmarkerResult 提取第一张脸的 478 个关键点。"""
        self.face_landmarks = None
        if result.face_landmarks:
            self.face_landmarks = result.face_landmarks[0]

    def get_detect_result(self) -> dict | None:
        if self.face_landmarks is None:
            return None
        return {
            'face_landmarks': self.face_landmarks,  # List[NormalizedLandmark]，478 个
        }
