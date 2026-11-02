"""Bounded presentation cache. No inference, rendering or camera writes."""
import copy
import math
import threading
import time
from phone_use_policy import PhoneUsePolicy, PHONE_MAX_AGE_S

ALGORITHMS = ('gesture_mantis', 'emotion', 'focus', 'phone_use')


class PerceptionState:
    def __init__(self):
        self._lock = threading.Lock()
        self._latest = None
        self._paired = None
        self._results = {}
        self._errors = set()
        self._phone_policy = PhoneUsePolicy()
        self._stream_id = 0

    def reset_video(self, stream_id):
        with self._lock:
            self._stream_id = stream_id
            self._latest = self._paired = None
            self._results.pop('gesture_mantis', None)

    def reset_phone(self):
        with self._lock:
            self._phone_policy.reset()
            self._results.pop('phone_use', None)
            self._errors.discard('phone_use')

    def interrupt_phone_hand(self):
        """A newer hand observation breaks overlap, without inventing a YOLO sample."""
        with self._lock:
            self._phone_policy.interrupt_overlap()
            cached = self._results.get('phone_use')
            if cached and isinstance(cached.get('result'), dict):
                result = cached['result']
                result.update(hand_overlap=None, sustained_ms=0,
                              status='detected' if result.get('phone_detected') else 'not_detected')
                return copy.deepcopy(result)
        return None

    def update_frame(self, image, seq, received_at):
        with self._lock:
            self._latest = {'image': image, 'frame_seq': seq, 'received_at': received_at}

    def commit(self, name, frame, response, completed_at=None):
        if name not in ALGORITHMS:
            return
        completed_at = time.monotonic() if completed_at is None else completed_at
        result = copy.deepcopy(response.get('result'))
        with self._lock:
            if name == 'phone_use':
                try:
                    result = self._phone_policy.observe(result, frame.frame_seq,
                                                        frame.received_at, completed_at)
                except ValueError:
                    self._errors.add(name)
                    return None
            self._errors.discard(name)
            self._results[name] = {'result': result, 'frame_seq': frame.frame_seq,
                                   'received_at': frame.received_at, 'completed_at': completed_at}
            if name == 'gesture_mantis':
                self._paired = {'image': frame.image_base64, 'frame_seq': frame.frame_seq,
                                'received_at': frame.received_at, 'result': result}
        return result

    def failed(self, name):
        if name in ALGORITHMS:
            with self._lock:
                self._errors.add(name)
                if name == 'phone_use':
                    self._phone_policy.reset()

    @staticmethod
    def _points(result):
        points = result.get('hand_landmarks') if isinstance(result, dict) else None
        if not result or result.get('hand_detected') is not True or not isinstance(points, list) or len(points) != 21:
            return None
        clean = []
        for point in points:
            if not isinstance(point, dict):
                return None
            x, y = point.get('x'), point.get('y')
            if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or v < 0 or v > 1 for v in (x, y)):
                return None
            # GestureRecognizer's existing service mirrors its input. The JPEG
            # here is the exact unmirrored input; only this overlay is mapped back.
            z = point.get('z', 0)
            if isinstance(z, bool) or not isinstance(z, (int, float)) or not math.isfinite(z):
                z = 0
            clean.append({'x': 1.0 - x, 'y': y, 'z': z})
        return clean

    def hand_overlay(self, enabled, healthy, connected, now=None):
        """Gesture metadata retains its own input identity, never the latest seq."""
        now = time.monotonic() if now is None else now
        with self._lock:
            paired = copy.deepcopy(self._paired)
            failed = 'gesture_mantis' in self._errors
            stream_id = self._stream_id
        age_ms = max(0, int((now - paired['received_at']) * 1000)) if paired else None
        usable = (connected and enabled.get('gesture_mantis') and not failed and
                  healthy.get('gesture_mantis') is not False and paired and age_ms <= 3000)
        points = self._points(paired['result']) if usable else None
        return {'type': 'hand_overlay', 'stream_id': stream_id,
                'frame_seq': paired['frame_seq'] if paired else None,
                'age_ms': age_ms, 'coordinate_space': 'normalized',
                'overlay_valid': points is not None, 'hand_landmarks': points}

    def snapshot(self, enabled, healthy, connected, now=None, include_image=True):
        now = time.monotonic() if now is None else now
        with self._lock:
            latest = copy.copy(self._latest)
            paired = copy.copy(self._paired)
            results = copy.deepcopy(self._results)
            errors = set(self._errors)
        age = lambda value: max(0, int((now - value) * 1000))
        camera_age = age(latest['received_at']) if latest else None
        camera_stale = not connected or latest is None or camera_age > 3000
        algorithms = {}
        for name in ALGORITHMS:
            cached = results.get(name)
            result = cached['result'] if cached else None
            enabled_now = bool(enabled.get(name))
            result_age = age(cached['completed_at']) if cached else None
            state = 'waiting'
            if not enabled_now:
                state = 'disabled'
            elif name in errors or healthy.get(name) is False:
                state = 'error'
            elif cached:
                state = 'ready'
                max_age_ms = PHONE_MAX_AGE_S * 1000 if name == 'phone_use' else 10000
                if camera_stale or result_age > max_age_ms or age(cached['received_at']) > max_age_ms:
                    state = 'stale'
                elif not isinstance(result, dict) or result.get('error'):
                    state = 'error'
                elif name == 'emotion' and result.get('type') in ('no_face', 'no_model', 'error'):
                    state = result['type']
                elif name == 'gesture_mantis' and not result.get('hand_detected'):
                    state = 'no_hand'
                elif name == 'focus':
                    sampling = result.get('sampling', {})
                    for field in ('classification_age_ms', 'face_age_ms'):
                        if isinstance(sampling.get(field), (int, float)):
                            sampling[field] += result_age
                    window = result.get('window', {})
                    state = window.get('state') if window.get('state') in ('collecting', 'waiting', 'insufficient_data') else 'ready'
                    if sampling.get('state') == 'no_face' or isinstance(sampling.get('face_age_ms'), (int, float)) and sampling['face_age_ms'] >= 3000:
                        state = 'no_face'
                    elif sampling.get('state') == 'warming_up':
                        state = 'collecting'
                    if isinstance(window.get('completed_age_ms'), (int, float)):
                        window['completed_age_ms'] += result_age
            algorithms[name] = {'enabled': enabled_now, 'state': state,
                                'frame_seq': cached['frame_seq'] if cached else None,
                                'age_ms': result_age, 'result': result}
        frame = None
        if not camera_stale:
            use_pair = (enabled.get('gesture_mantis') and paired and
                        'gesture_mantis' not in errors and healthy.get('gesture_mantis') is not False and
                        paired['frame_seq'] == latest['frame_seq'] and
                        age(paired['received_at']) <= 3000)
            selected = latest
            points = self._points(paired['result']) if use_pair else None
            frame = {'image': selected['image'], 'frame_seq': selected['frame_seq'],
                     'age_ms': age(selected['received_at']), 'hand_landmarks': points,
                     'overlay_valid': points is not None, 'coordinate_space': 'normalized'}
            if not include_image:
                frame.pop('image')
        return {'success': True, 'schema_version': 1,
                'camera': {'connected': bool(connected), 'stale': camera_stale,
                           'frame_seq': latest['frame_seq'] if latest else None, 'age_ms': camera_age},
                'frame': frame, 'algorithms': algorithms,
                'hand_overlay': self.hand_overlay(enabled, healthy, connected, now)}
