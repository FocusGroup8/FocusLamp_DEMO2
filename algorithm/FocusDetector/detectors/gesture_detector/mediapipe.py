"""
MediaPipe GestureRecognizer 手势检测器

使用 LIVE_STREAM 异步模式，识别 7 种手势 + 输出 21 个手部关键点。

输出接口：
  run(image)                异步提交一帧
  get_detect_result() -> dict | None
    {
      'gesture': 'Open_Palm',
      'hand_landmarks': [[(x0, y0), ..., (x20, y20)]],  # 每只手 21 个归一化关键点
    }
"""

import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from detectors.gesture_detector.base import GestureDetectorProviderBase

TAG = __name__


class GestureDetectorProvider(GestureDetectorProviderBase):
    def __init__(
        self,
        model_path: str = "checkpoint/gesture_recognizer.task",
        num_hands: int = 1,
        min_hand_detection_confidence: float = 0.5,
        min_hand_presence_confidence: float = 0.5,
        min_tracking_confidence: float = 0.5,
    ):
        super().__init__()
        try:
            self.gesture_category: str = None
            self.gesture_score: float = 0.0

            base_options = python.BaseOptions(model_asset_path=model_path)
            options = vision.GestureRecognizerOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                num_hands=num_hands,
                min_hand_detection_confidence=min_hand_detection_confidence,
                min_hand_presence_confidence=min_hand_presence_confidence,
                min_tracking_confidence=min_tracking_confidence,
                result_callback=self._make_callback(),
            )
            self._landmarker = vision.GestureRecognizer.create_from_options(options)
        except Exception as e:
            self.logger.bind(tag=TAG).error(f'Failed to initialize GestureDetectorProvider: {e}')
            raise e

    def _submit_async(self, mp_image, timestamp_ms: int):
        # GestureRecognizer 用 recognize_async（注意方法名不同于 detect_async）
        self._landmarker.recognize_async(mp_image, timestamp_ms)

    def _extract_result(self, result) -> None:
        """从 GestureRecognizerResult 提取手势类别和分数。"""
        self.gesture_category = None
        self.gesture_score = 0.0
        if result.gestures:
            gestures = result.gestures[0]
            if gestures:
                gesture = gestures[0]
                self.gesture_category = gesture.category_name
                self.gesture_score = gesture.score

    def get_detect_result(self) -> dict | None:
        if self.gesture_category is None:
            return None
        # 提取手部关键点（每只手 21 个归一化坐标）
        landmarks = []
        if self.recognition_result and self.recognition_result.hand_landmarks:
            for hand in self.recognition_result.hand_landmarks:
                hand_pts = [(lm.x, lm.y) for lm in hand]
                landmarks.append(hand_pts)
        return {
            'gesture': self.gesture_category,
            'hand_landmarks': landmarks,
        }
