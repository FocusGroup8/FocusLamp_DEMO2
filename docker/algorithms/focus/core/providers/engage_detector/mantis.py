from __future__ import annotations
import time
import threading
import numpy as np
import onnxruntime as ort
import mediapipe as mp
import torch
import torch.nn.functional as F

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from core.providers.engage_detector.base import EngageDetectorProviderBase

TAG = __name__

class EngageDetectorProvider(EngageDetectorProviderBase):
    def __init__(self, config_path: str):
        super().__init__(config_path=config_path)
        try:
            mediapipe_model = self.config.get('mediapipe_model', 'face_landmarker.task')
            num_faces = self.config.get('num_faces', 1)
            min_face_detection_confidence = self.config.get('min_face_detection_confidence', 0.5)
            min_face_presence_confidence = self.config.get('min_face_presence_confidence', 0.5)
            min_tracking_confidence = self.config.get('min_tracking_confidence', 0.5)

            self.detection_result = None
            self.features_queue = []
            self.features_lock = threading.Lock()
            self.last_inference_result = None
            self._timestamp_ms = int(time.time() * 1000)
            self._timestamp_lock = threading.Lock()

            def save_result(result: vision.FaceLandmarkerResult, unused_output_image: mp.Image, timestamp_ms: int):
                try:
                    self.detection_result = result

                    face_blendshapes = None
                    if self.detection_result:
                        face_blendshapes = self.detection_result.face_blendshapes

                    if face_blendshapes:
                        blendshapes = [item.score for item in face_blendshapes[0]]
                        with self.features_lock:
                            self.features_queue.append(blendshapes)
                except Exception as e:
                    self.logger.error(f'Failed to save result in core.providers.engage_detector.mantis: {e}')
                    return

            base_options = python.BaseOptions(model_asset_path=mediapipe_model)
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

            self.label_map = {
                0: 'Highly-Engaged',
                1: 'Engaged',
                2: 'Barely-engaged',
                3: 'Not-Engaged',
            }
            self.engage_detector = ort.InferenceSession(self.config.get('engage_detector_model', 'engage_model.onnx'))
            print(f"[INFO] EngageDetectorProvider loaded (LIVE_STREAM mode), ONNX providers: {self.engage_detector.get_providers()}", flush=True)
        except Exception as e:
            self.logger.error(f'Failed to initialize EngageDetectorProvider: {e}')
            raise e

    def _next_timestamp(self) -> int:
        with self._timestamp_lock:
            self._timestamp_ms += 1
            now_ms = int(time.time() * 1000)
            if self._timestamp_ms < now_ms:
                self._timestamp_ms = now_ms
            return self._timestamp_ms

    def run(self, image: np.ndarray):
        try:
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
            self.face_landmarker.detect_async(mp_image, self._next_timestamp())
        except Exception as e:
            self.logger.error(f'Failed to run EngageDetectorProvider: {e}')
            return

    def get_detect_result(self) -> dict | None:
        with self.features_lock:
            if len(self.features_queue) >= 300:
                features = np.array(self.features_queue, dtype=np.float32)
                features = features.reshape(1, -1, 52)
                features = torch.tensor(features, dtype=torch.float32)
                features = features.transpose(1, 2)
                features = F.interpolate(features, size=512, mode='linear', align_corners=False)
                result = self.engage_detector.run(None, {"input": features.numpy()})
                self.features_queue = self.features_queue[150:]
                engage_level = np.argmax(result[0])
                engage_level_name = self.label_map[engage_level]

                self.last_inference_result = {
                    'engage_level': engage_level.item(),
                    'engage_level_name': engage_level_name,
                }

                print(f"[INFERENCE] level={engage_level.item()} ({engage_level_name})", flush=True)
                return self.last_inference_result
        return None

    def close(self):
        try:
            self.face_landmarker.close()
            del self.engage_detector
            self.face_landmarker = None
            self.engage_detector = None
        except Exception as e:
            self.logger.error(f'EngageDetectorProvider close failed: {e}')
            return
