from __future__ import annotations
import time
import uuid
import threading
import math
import numpy as np
import onnxruntime as ort
import mediapipe as mp
import torch
import torch.nn.functional as F

from mediapipe.tasks import python
from mediapipe.tasks.python import vision

from core.providers.engage_detector.base import EngageDetectorProviderBase

TAG = __name__

def orientation_from_landmarks(points):
    """Shadow observation only: 2D nose offset along the eye line, not gaze.
    Thresholds are provisional geometry settings, not learned focus labels.
    """
    result={'source':'mediapipe_landmark_geometry_v1','shadow':True,'controls_timer':False,
            'state':'unknown','reason':'no_landmarks','yaw_proxy':None,
            'facing_threshold':0.20,'turned_threshold':0.35}
    if not points or len(points)<=263:return result
    left,right,nose=points[33],points[263],points[1]
    values=[p.x for p in (left,right,nose)]+[p.y for p in (left,right,nose)]
    if not all(math.isfinite(v) and 0<=v<=1 for v in values):
        return dict(result,reason='invalid_landmarks')
    dx,dy=right.x-left.x,right.y-left.y
    span2=dx*dx+dy*dy
    if span2<0.025**2:return dict(result,reason='face_too_small')
    offset=((nose.x-(left.x+right.x)/2)*dx+(nose.y-(left.y+right.y)/2)*dy)/span2
    magnitude=abs(offset)
    state='facing_camera' if magnitude<=0.20 else 'turned' if magnitude>=0.35 else 'uncertain'
    return dict(result,state=state,reason='geometry_only',yaw_proxy=float(offset))

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
            self.orientation_seq=0
            self.orientation_shadow=orientation_from_landmarks(None)
            self.last_orientation_at=None
            self.orientation_last_attempt=None
            self.features_queue = []
            self.features_lock = threading.Lock()
            self.last_inference_result = None
            self.valid_samples = 0
            self.last_inference_samples = 0
            self.next_score_sample = 300
            self.last_face_at = None
            self.last_inference_at = None
            self.source_epoch = uuid.uuid4().hex
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
                            self.features_queue = self.features_queue[-300:]
                            self.valid_samples += 1
                            self.last_face_at = time.monotonic()
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
            # Shadow only: reacquire independently so a failed temporal track
            # cannot suppress a face recoverable in the current image.
            self.orientation_landmarker=vision.FaceLandmarker.create_from_options(
                vision.FaceLandmarkerOptions(base_options=python.BaseOptions(model_asset_path=mediapipe_model),
                    running_mode=vision.RunningMode.IMAGE,num_faces=1,
                    min_face_detection_confidence=min_face_detection_confidence,
                    min_face_presence_confidence=min_face_presence_confidence,
                    min_tracking_confidence=min_tracking_confidence))

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
            now=time.monotonic()
            if self.orientation_last_attempt is None or now-self.orientation_last_attempt>=0.5:
                self.orientation_last_attempt=now
                result=self.orientation_landmarker.detect(mp_image)
                orientation=orientation_from_landmarks(result.face_landmarks[0] if result.face_landmarks else None)
                self.orientation_seq+=1
                orientation.update(sample_seq=self.orientation_seq,source='mediapipe_image_landmark_geometry_v1',
                                   detection_ms=round((time.monotonic()-now)*1000,1),max_fps=2)
                with self.features_lock:
                    self.orientation_shadow=orientation;self.last_orientation_at=now
        except Exception as e:
            self.logger.error(f'Failed to run EngageDetectorProvider: {e}')
            return

    def get_detect_result(self) -> dict | None:
        with self.features_lock:
            if (len(self.features_queue) < 300 or
                    (self.last_inference_result is not None and self.valid_samples - self.last_inference_samples < 30)):
                return None
            # Snapshot only. MediaPipe callbacks must not wait for ONNX inference.
            features = np.array(self.features_queue[-300:], dtype=np.float32)
            sample_count = self.valid_samples
        features = torch.tensor(features.reshape(1, -1, 52), dtype=torch.float32).transpose(1, 2)
        features = F.interpolate(features, size=512, mode='linear', align_corners=False)
        result = self.engage_detector.run(None, {"input": features.numpy()})
        # Observe the existing model contract without rescaling inputs,
        # remapping classes or turning arbitrary scores into confidence.
        diagnostics = None
        model_input = features.numpy()
        if hasattr(model_input, 'shape') and hasattr(result[0], 'tolist'):
            diagnostics = {
                'input_shape': list(model_input.shape),
                'feature_min': float(model_input.min()),
                'feature_max': float(model_input.max()),
                'feature_mean': float(model_input.mean()),
                'feature_std': float(model_input.std()),
                'temporal_std_mean': float(model_input.std(axis=2).mean()),
                'raw_class_scores': result[0].reshape(-1).tolist(),
                'class_order': [self.label_map[i] for i in range(4)],
            }
        engage_level = np.argmax(result[0])
        engage_level_name = self.label_map[engage_level]
        with self.features_lock:
            self.last_inference_samples = sample_count
            self.last_inference_at = time.monotonic()
            self.last_inference_result = {'engage_level': engage_level.item(), 'engage_level_name': engage_level_name}
            if diagnostics is not None:
                self.last_inference_result['model_diagnostics'] = diagnostics
            # Faster rolling previews must not inflate the 90-second score's
            # sample count. Keep its original first-300 / next-150 cadence.
            score_due = sample_count >= self.next_score_sample
            if score_due:
                self.next_score_sample = 300 + ((sample_count - 300) // 150 + 1) * 150
            output = dict(self.last_inference_result, _score_sample_due=score_due)
        print(f"[INFERENCE] level={engage_level.item()} ({engage_level_name}) samples={sample_count} score_sample={score_due}", flush=True)
        return output

    def sampling_metadata(self):
        now = time.monotonic()
        with self.features_lock:
            face_age = None if self.last_face_at is None else max(0, int((now-self.last_face_at)*1000))
            classification_age = None if self.last_inference_at is None else max(0, int((now-self.last_inference_at)*1000))
            orientation_age=None if self.last_orientation_at is None else max(0,int((now-self.last_orientation_at)*1000))
            shadow=dict(self.orientation_shadow, age_ms=orientation_age)
            if orientation_age is None or orientation_age>=3000:
                shadow.update(state='unknown',reason='no_face_or_stale',yaw_proxy=None)
            state = 'no_face' if face_age is None or face_age >= 3000 else 'warming_up' if self.last_inference_result is None else 'ready'
            return {'source_epoch': self.source_epoch, 'inference_seq': self.last_inference_samples,
                    'orientation_shadow': shadow,
                    'state': state, 'valid_samples': self.valid_samples,
                    'buffered_samples': len(self.features_queue), 'required_samples': 300,
                    'next_inference_samples': max(0, 300-len(self.features_queue)) if self.last_inference_result is None else max(0, 30-(self.valid_samples-self.last_inference_samples)),
                    'classification_age_ms': classification_age, 'face_age_ms': face_age,
                    'inference_stride': 30, 'score_stride': 150}

    def reset(self):
        with self.features_lock:
            self.features_queue = []
            self.orientation_seq=0
            self.orientation_shadow=orientation_from_landmarks(None)
            self.last_orientation_at=None
            self.orientation_last_attempt=None
            self.last_inference_result = None
            self.valid_samples = self.last_inference_samples = 0
            self.next_score_sample = 300
            self.last_face_at = self.last_inference_at = None
            self.source_epoch = uuid.uuid4().hex

    def close(self):
        try:
            self.face_landmarker.close()
            self.orientation_landmarker.close()
            del self.engage_detector
            self.face_landmarker = None
            self.engage_detector = None
        except Exception as e:
            self.logger.error(f'EngageDetectorProvider close failed: {e}')
            return

