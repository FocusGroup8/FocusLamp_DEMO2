from __future__ import annotations
import time
import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from core.providers.eye_detector.base import EyeDetectorProviderBase

TAG = __name__

class EyeDetectorProvider(EyeDetectorProviderBase):
    def __init__(self, config_path: str):
        super().__init__(config_path=config_path)
        try:
            model = self.config.get('model', 'face_landmarker.task')
            num_faces = self.config.get('num_faces', 1)
            min_face_detection_confidence = self.config.get('min_face_detection_confidence', 0.5)
            min_face_presence_confidence = self.config.get('min_face_presence_confidence', 0.5)
            min_tracking_confidence = self.config.get('min_tracking_confidence', 0.5)

            self.detection_result = None
            self.horizontal_speed = 0.0
            self.vertical_speed = 0.0

            def save_result(result: vision.FaceLandmarkerResult, unused_output_image: mp.Image, timestamp_ms: int):
                try:
                    self.horizontal_speed = 0.0
                    self.vertical_speed = 0.0
                    self.detection_result = result

                    if self.detection_result:
                        face_blendshapes = self.detection_result.face_blendshapes

                    if face_blendshapes:
                        eye_blendshapes = face_blendshapes[0][9:23]
                        self._get_eye_speed(eye_blendshapes)
                except Exception as e:
                    self.logger.error(f'Failed to save result in core.providers.eye_detector.mediapipe: {e}')
                    return

            base_options = python.BaseOptions(model_asset_path=model)
            options = vision.FaceLandmarkerOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                num_faces=num_faces,
                min_face_detection_confidence=min_face_detection_confidence,
                min_face_presence_confidence=min_face_presence_confidence,
                min_tracking_confidence=min_tracking_confidence,
                output_face_blendshapes=True,
                result_callback=save_result,
            )

            self.face_landmarker = vision.FaceLandmarker.create_from_options(options)
            print("[INFO] EyeDetectorProvider loaded (LIVE_STREAM mode)", flush=True)
        except Exception as e:
            self.logger.error(f'Failed to initialize EyeDetectorProvider: {e}')
            raise e

    def run(self, image: np.ndarray):
        try:
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
            self.face_landmarker.detect_async(mp_image, time.time_ns() // 1_000_000)
        except Exception as e:
            self.logger.error(f'Failed to run EyeDetectorProvider: {e}')
            return

    def _get_eye_speed(self, eye_blendshapes, threshold: float = 0.5):
        try:
            right_speed = 0.0
            left_speed = 0.0
            down_speed = 0.0
            up_speed = 0.0

            for category in eye_blendshapes:
                category_name = category.category_name
                score = category.score

                if category_name == "eyeLookOutRight" and score > threshold:
                    right_speed += score
                if category_name == "eyeLookInLeft" and score > threshold:
                    right_speed += score

                if category_name == "eyeLookOutLeft" and score > threshold:
                    left_speed += score
                if category_name == "eyeLookInRight" and score > threshold:
                    left_speed += score

                if category_name in ["eyeLookDownLeft", "eyeLookDownRight"] and score > threshold:
                    down_speed += score

                if category_name in ["eyeLookUpLeft", "eyeLookUpRight"] and score > threshold:
                    up_speed += score

            self.horizontal_speed = left_speed - right_speed
            self.vertical_speed = down_speed - up_speed
        except Exception as e:
            self.logger.error(f'Failed to get eye speed in core.providers.eye_detector.mediapipe: {e}')
            return

    def get_detect_result(self) -> dict | None:
        return {
            'horizontal_speed': self.horizontal_speed,
            'vertical_speed': self.vertical_speed,
        }

    def close(self):
        try:
            self.face_landmarker.close()
            self.face_landmarker = None
        except Exception as e:
            self.logger.error(f'EyeDetectorProvider close failed: {e}')
            return
