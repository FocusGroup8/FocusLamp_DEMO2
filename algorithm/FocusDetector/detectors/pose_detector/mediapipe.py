"""
MediaPipe PoseLandmarker 肢体检测器

使用 LIVE_STREAM 异步模式，检测 33 个肢体关键点（含 z 深度和 visibility）。

输出接口：
  run(image)                异步提交一帧
  get_detect_result() -> dict | None
    {
      'pose_landmarks': [(x, y, z, visibility), ...],  # 33 个关键点
    }
"""

import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from detectors.pose_detector.base import PoseDetectorProviderBase

TAG = __name__


class PoseDetectorProvider(PoseDetectorProviderBase):
    def __init__(
        self,
        model_path: str = "checkpoint/pose_landmarker_full.task",
        min_pose_detection_confidence: float = 0.5,
        min_pose_presence_confidence: float = 0.5,
        min_tracking_confidence: float = 0.5,
    ):
        super().__init__()
        try:
            self.pose_landmarks = None

            base_options = python.BaseOptions(model_asset_path=model_path)
            options = vision.PoseLandmarkerOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                min_pose_detection_confidence=min_pose_detection_confidence,
                min_pose_presence_confidence=min_pose_presence_confidence,
                min_tracking_confidence=min_tracking_confidence,
                result_callback=self._make_callback(),
            )
            self._landmarker = vision.PoseLandmarker.create_from_options(options)
        except Exception as e:
            self.logger.bind(tag=TAG).error(f'Failed to initialize PoseDetectorProvider: {e}')
            raise e

    def _submit_async(self, mp_image, timestamp_ms: int):
        self._landmarker.detect_async(mp_image, timestamp_ms)

    def _extract_result(self, result) -> None:
        """从 PoseLandmarkerResult 提取 33 个关键点。"""
        self.pose_landmarks = None
        if result.pose_landmarks:
            self.pose_landmarks = result.pose_landmarks[0]

    def get_detect_result(self) -> dict | None:
        if self.pose_landmarks is None:
            return None
        # 提取 33 个关键点（含 z 深度 + visibility）
        landmarks = [(lm.x, lm.y, lm.z, lm.visibility) for lm in self.pose_landmarks]
        return {
            'pose_landmarks': landmarks,
        }
