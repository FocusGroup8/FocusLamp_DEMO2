"""CPU-only local phone detector. No cloud client, image output or hand model."""
import base64
import binascii
import logging
import os
from pathlib import Path
import threading
import time

# Set these before importing Ultralytics: its online checks and auto-install
# helpers must remain disabled. Only an existing local .pt file is accepted.
os.environ['YOLO_OFFLINE'] = 'true'
os.environ['YOLO_AUTOINSTALL'] = 'false'
os.environ.setdefault('YOLO_CONFIG_DIR', '/tmp/phone-ultralytics')
os.environ.setdefault('OMP_NUM_THREADS', '2')
os.environ.setdefault('MKL_NUM_THREADS', '2')

from flask import Flask, jsonify, request
from phone_logic import build_result, finite_number, normalize_detections

logger = logging.getLogger('phone_detector')


class ModelUnavailable(Exception):
    pass


class DetectionBusy(Exception):
    pass


class PhoneRuntime:
    def __init__(self, predictor=None, threshold=None, margin_px=None):
        self.lock = threading.Lock()
        self.threshold = float(os.getenv('PHONE_CONFIDENCE_THRESHOLD', '0.35')) if threshold is None else threshold
        self.margin_px = float(os.getenv('HAND_CONTACT_MARGIN', '25')) if margin_px is None else margin_px
        if not finite_number(self.threshold) or not 0 <= self.threshold <= 1:
            raise ValueError('PHONE_CONFIDENCE_THRESHOLD must be within 0..1')
        if not finite_number(self.margin_px) or not 0 <= self.margin_px <= 100:
            raise ValueError('HAND_CONTACT_MARGIN must be within 0..100 pixels')
        self.predictor = predictor
        self.model_error = None
        if predictor is None:
            self._load_model()

    def _load_model(self):
        try:
            model_path = Path(os.getenv('YOLO_MODEL', '/app/yolo26s.pt')).resolve(strict=True)
            if not model_path.is_file() or model_path.suffix.lower() != '.pt':
                raise ValueError('YOLO_MODEL must point to an existing local .pt file')
            import torch
            threads = max(1, min(8, int(os.getenv('PHONE_INFERENCE_THREADS', '2'))))
            torch.set_num_threads(threads)
            try:
                torch.set_num_interop_threads(1)
            except RuntimeError:
                pass  # An embedding process may already have initialized Torch.
            from ultralytics import YOLO, settings
            settings.update({'sync': False})
            model = YOLO(str(model_path), task='detect')

            def predict(image):
                results = model.predict(image, device='cpu', classes=[67], conf=self.threshold,
                                        imgsz=640, max_det=10, verbose=False, save=False, show=False)
                rows = []
                for result in results:
                    if result.boxes is not None:
                        rows.extend(result.boxes.data.cpu().tolist())
                return rows

            self.predictor = predict
            logger.info('Local phone model loaded: %s (CPU threads=%s)', model_path, threads)
        except Exception as error:
            self.model_error = str(error)
            logger.error('Local phone model unavailable: %s', error)

    @property
    def ready(self):
        return self.predictor is not None

    def detect(self, image, payload):
        if not self.lock.acquire(blocking=False):
            raise DetectionBusy('Inference busy; skip this frame')
        try:
            if not self.ready:
                raise ModelUnavailable(self.model_error or 'Local model unavailable')
            height, width = image.shape[:2]
            detections = normalize_detections(self.predictor(image), width, height, self.threshold)
            return build_result(detections, payload, width, height, self.margin_px)
        finally:
            self.lock.release()


def decode_image(value):
    if not isinstance(value, str) or not value:
        raise ValueError('Missing image')
    if value.startswith('data:image/'):
        prefix, separator, value = value.partition(',')
        if not separator or not prefix.endswith(';base64'):
            raise ValueError('Invalid image data URL')
    try:
        encoded = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError) as error:
        raise ValueError('Invalid image base64') from error
    if not encoded:
        raise ValueError('Empty image')
    import cv2
    import numpy as np
    try:
        image = cv2.imdecode(np.frombuffer(encoded, np.uint8), cv2.IMREAD_COLOR)
    except cv2.error as error:
        raise ValueError('Image decode failed') from error
    if image is None:
        raise ValueError('Image decode failed')
    return image


def create_app(runtime=None, decoder=None):
    runtime = runtime if runtime is not None else PhoneRuntime()
    decoder = decoder or decode_image
    app = Flask(__name__)
    app.config['MAX_CONTENT_LENGTH'] = 8 * 1024 * 1024

    @app.get('/health')
    def health():
        return jsonify({'status': 'healthy' if runtime.ready else 'unavailable',
                        'model_loaded': runtime.ready, 'local_only': True,
                        'error': None if runtime.ready else runtime.model_error}), 200 if runtime.ready else 503

    @app.post('/detect')
    def detect():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({'success': False, 'error': 'JSON object required'}), 400
        if not runtime.ready:
            return jsonify({'success': False, 'error': runtime.model_error or 'Local model unavailable',
                            'state': 'no_model'}), 503
        started = time.monotonic()
        try:
            image = decoder(payload.get('image'))
        except ValueError as error:
            return jsonify({'success': False, 'error': str(error)}), 400
        try:
            result = runtime.detect(image, payload)
            return jsonify({'success': True, 'result': result,
                            'processing_time': time.monotonic() - started})
        except (DetectionBusy, ModelUnavailable) as error:
            return jsonify({'success': False, 'error': str(error)}), 503
        except Exception:
            logger.exception('Local phone inference failed')
            return jsonify({'success': False, 'error': 'Local phone inference failed'}), 500

    return app


app = create_app()
