from __future__ import annotations
import time
import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from core.providers.gesture_detector.base import GestureDetectorProviderBase

TAG = __name__

class GestureDetectorProvider(GestureDetectorProviderBase):
    def __init__(self, config_path: str):
        super().__init__(config_path=config_path)
        try:
            model = self.config.get('model', 'gesture_recognizer.task')
            num_hands = self.config.get('num_hands', 1)
            min_hand_detection_confidence = self.config.get('min_hand_detection_confidence', 0.5)
            min_hand_presence_confidence = self.config.get('min_hand_presence_confidence', 0.5)
            min_tracking_confidence = self.config.get('min_tracking_confidence', 0.5)

            self.gesture_category: str = None
            self.gesture_score: float = 0.0

            self.recognition_result = None

            def save_result(result: vision.GestureRecognizerResult, unused_output_image: mp.Image, timestamp_ms: int):
                try:
                    self.gesture_category = None
                    self.gesture_score = 0.0
                    self.recognition_result = result

                    if self.recognition_result.gestures:
                        gestures = self.recognition_result.gestures[0]
                        if gestures:
                            gesture = gestures[0]
                            self.gesture_category = gesture.category_name
                            self.gesture_score = gesture.score
                except Exception as e:
                    self.logger.error(f'Failed to save result in core.providers.gesture_detector.mediapipe: {e}')
                    return

            base_options = python.BaseOptions(model_asset_path=model)
            options = vision.GestureRecognizerOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                num_hands=num_hands,
                min_hand_detection_confidence=min_hand_detection_confidence,
                min_hand_presence_confidence=min_hand_presence_confidence,
                min_tracking_confidence=min_tracking_confidence,
                result_callback=save_result,
            )
            self.gesture_recognizer = vision.GestureRecognizer.create_from_options(options)
            print("[INFO] GestureDetectorProvider loaded (LIVE_STREAM mode)", flush=True)
        except Exception as e:
            self.logger.error(f'Failed to initialize GestureDetectorProvider: {e}')
            raise e

    def run(self, image: np.ndarray):
        try:
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
            self.gesture_recognizer.recognize_async(mp_image, time.time_ns() // 1_000_000)
        except Exception as e:
            self.logger.error(f'Failed to run GestureDetectorProvider: {e}')
            return

    def get_detect_result(self) -> dict | None:
        if self.gesture_category is None:
            return None
        return {
            'gesture': self.gesture_category,
            'score': self.gesture_score,
        }

    def close(self):
        try:
            self.gesture_recognizer.close()
        except Exception as e:
            self.logger.error(f'GestureDetectorProvider close failed: {e}')
