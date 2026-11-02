"""Experimental advice only. Never writes controller state or transport payloads."""
import time
from collections import deque


class FusionShadow:
    def __init__(self):
        self.reset(None)

    def reset(self, source):
        self.source = source
        self.orientation_seq = None
        self.orientation = 'unknown'
        self.candidate = None
        self.count = 0
        self.since = 0
        self.orientation_last = None
        self.model_seq = None
        self.model = deque()
        self.phone_seq = None
        self.misses = None
        self.phone = 'unknown'

    def evaluate(self, snapshot, now=None):
        now = time.monotonic() if now is None else now
        algorithms = snapshot.get('algorithms', {})
        focus = algorithms.get('focus', {})
        sample = (focus.get('result') or {}).get('sampling', {})
        source = (snapshot.get('hand_overlay', {}).get('stream_id'), sample.get('source_epoch'))
        if source != self.source:
            self.reset(source)
        missing = []
        camera = snapshot.get('camera', {})
        def fresh(age, ttl):
            return isinstance(age, (int, float)) and 0 <= age < ttl
        face_ok = fresh(sample.get('face_age_ms'), 3000)
        required = (camera.get('connected') is True and camera.get('stale') is False and
                    fresh(camera.get('age_ms'), 3000) and focus.get('enabled') is True and
                    focus.get('state') == 'ready' and face_ok)
        shadow = sample.get('orientation_shadow', {})
        age = shadow.get('age_ms')
        residence = focus.get('age_ms')
        orientation_ok = (required and fresh(age, 3000) and fresh(residence, 3000) and
                          age + residence < 3000 and shadow.get('state') in ('facing_camera', 'turned', 'uncertain'))
        if not orientation_ok:
            self.orientation = 'unknown'
            self.candidate = None
            self.count = 0
            missing.append('orientation:invalid_or_stale')
        elif shadow.get('sample_seq') is None:
            self.orientation = 'unknown'
            missing.append('orientation:missing_sample_seq')
        elif shadow['sample_seq'] != self.orientation_seq:
            self.orientation_seq = shadow['sample_seq']
            if self.orientation_last is not None and now - self.orientation_last > 2:
                self.candidate = None
                self.count = 0
                self.orientation = 'unknown'
            self.orientation_last = now
            state = shadow['state']
            if state == 'uncertain':
                self.orientation = 'uncertain'
                self.candidate = None
                self.count = 0
            else:
                if state != self.candidate or now - self.since > 12:
                    self.candidate, self.count, self.since = state, 0, now
                self.count += 1
                if self.count >= 3 and now - self.since >= 1:
                    self.orientation = state
        seq = sample.get('inference_seq')
        model_ok = required and fresh(sample.get('classification_age_ms'), 15000)
        name = (focus.get('result') or {}).get('engage_detect_result', {}).get('engage_level_name', '')
        name = name.lower().replace('-', '').replace('_', '')
        verdict = 'aligned' if name in ('highlyengaged', 'engaged') else 'diverge' if name == 'notengaged' else 'insufficient'
        if not model_ok:
            self.model.clear()
            missing.append('focus:invalid_or_stale')
        elif seq is not None and seq != self.model_seq:
            self.model_seq = seq
            if self.model and now - self.model[-1][0] > 15:
                self.model.clear()
            self.model.append((now, verdict))
        while self.model and now - self.model[0][0] > 30:
            self.model.popleft()
        confirmed = (len(self.model) >= 2 and self.model[-1][1] == self.model[-2][1])
        group_a = 'unknown' if not required else 'insufficient'
        if confirmed and self.orientation in ('facing_camera', 'turned'):
            orientation_verdict = 'aligned' if self.orientation == 'facing_camera' else 'diverge'
            group_a = verdict if verdict == orientation_verdict else 'conflict'
        phone = algorithms.get('phone_use', {})
        p = (phone.get('result') or {})
        hand = algorithms.get('gesture_mantis', {})
        phone_ok = (phone.get('enabled') is True and fresh(phone.get('age_ms'), 3000) and
                    hand.get('state') != 'no_hand' and fresh(hand.get('age_ms'), 3000))
        if not phone_ok:
            self.phone = 'unknown'
            self.misses = None
            self.phone_seq = None
            missing.append('phone_use:stale_or_no_hand')
        elif phone.get('frame_seq') is not None and phone.get('frame_seq') != self.phone_seq:
            self.phone_seq = phone.get('frame_seq')
            misses = p.get('consecutive_misses')
            steady = self.misses is not None and isinstance(misses, int) and misses <= self.misses
            self.misses = misses
            current = (p.get('phone_detected') is True and p.get('hand_overlap') is True and
                       p.get('presence_state') not in (None, 'not_detected'))
            duration = p.get('sustained_ms', 0)
            threshold = p.get('confirmation_ms', 3000)
            self.phone = ('using' if current and steady and threshold >= 3000 and duration >= threshold else
                          'present_only' if p.get('phone_detected') is True else 'unknown')
        suggest = ('grey' if not required else 'red' if group_a == 'diverge' and self.phone == 'using' else
                   'green' if group_a == 'aligned' and self.phone == 'unknown' else 'amber')
        return dict(version=1, shadow=True, controls_timer=False, suggest=suggest,
                    group_a=dict(verdict=group_a, orientation=self.orientation, model=verdict,
                                 model_samples=len(self.model)), group_b=dict(verdict=self.phone),
                    available_groups=int(required and model_ok and orientation_ok) + int(phone_ok),
                    supporting_groups=int(group_a == 'diverge') + int(self.phone == 'using'),
                    grey_reason=None if required else 'required_data_unavailable', missing=missing,
                    policy='phone_prohibited', parameters_status='trial_not_calibrated')
