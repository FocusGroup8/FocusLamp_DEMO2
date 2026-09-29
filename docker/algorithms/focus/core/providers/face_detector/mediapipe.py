from __future__ import annotations
import time
import numpy as np
import mediapipe as mp

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from core.providers.face_detector.base import FaceDetectorProviderBase

TAG = __name__

class FaceDetectorProvider(FaceDetectorProviderBase):
    def __init__(self, config_path: str):
        super().__init__(config_path=config_path)
        try:
            model = self.config.get('model', 'blaze_face_short_range.tflite')
            min_detection_confidence = self.config.get('min_detection_confidence', 0.5)
            min_suppression_threshold = self.config.get('min_suppression_threshold', 0.3)

            self.offset_dead_zone = self.config.get('offset_dead_zone', 0.1)

            self.image_width = None
            self.image_height = None

            self.face_detection_result = None

            def save_result(result: vision.FaceDetectorResult, unused_output_image: mp.Image, timestamp_ms: int):
                try:
                    self.face_detection_result = result
                except Exception as e:
                    self.logger.error(f'Failed to save result in core.providers.face_detector.mediapipe: {e}')
                    return

            base_options = python.BaseOptions(model_asset_path=model)
            options = vision.FaceDetectorOptions(
                base_options=base_options,
                running_mode=vision.RunningMode.LIVE_STREAM,
                min_detection_confidence=min_detection_confidence,
                min_suppression_threshold=min_suppression_threshold,
                result_callback=save_result,
            )
            self.face_detector = vision.FaceDetector.create_from_options(options)
            print("[INFO] FaceDetectorProvider loaded (LIVE_STREAM mode)", flush=True)

        except Exception as e:
            self.logger.error(f'Failed to initialize FaceDetectorProvider: {e}')
            raise e

    def run(self, image: np.ndarray):
        try:
            if self.image_height is None or self.image_width is None:
                self.image_height, self.image_width = image.shape[:2]

            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
            self.face_detector.detect_async(mp_image, time.time_ns() // 1_000_000)
        except Exception as e:
            self.logger.error(f'Failed to run FaceDetectorProvider: {e}')
            return

    def get_detect_result(self):
        try:
            if self.face_detection_result is None:
                return None

            detections = []
            for detection in self.face_detection_result.detections:
                bbox = detection.bounding_box
                if bbox is None:
                    continue

                face_center_x = bbox.origin_x + bbox.width / 2
                face_center_y = bbox.origin_y + bbox.height / 2

                offset_x = face_center_x - self.image_width / 2
                offset_y = face_center_y - self.image_height / 2

                normalized_offset_x = offset_x / (self.image_width / 2)
                normalized_offset_y = offset_y / (self.image_height / 2)

                if abs(normalized_offset_x) < self.offset_dead_zone:
                    normalized_offset_x = 0.0
                if abs(normalized_offset_y) < self.offset_dead_zone:
                    normalized_offset_y = 0.0

                detections.append({
                    'normalized_offset_x': normalized_offset_x,
                    'normalized_offset_y': normalized_offset_y,
                })

            if len(detections) > 0:
                return {
                    'normalized_offset_x': detections[0]['normalized_offset_x'],
                    'normalized_offset_y': detections[0]['normalized_offset_y'],
                }
            else:
                return None
        except Exception as e:
            self.logger.error(f'Failed to get detect result in core.providers.face_detector.mediapipe: {e}')
            return None

    def close(self):
        try:
            self.face_detector.close()
        except Exception as e:
            self.logger.error(f'FaceDetectorProvider closed failed: {e}')
            return
