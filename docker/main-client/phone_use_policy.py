"""Local presentation policy; no reminder, network calls or image inference."""
import copy
import math


PHONE_MAX_AGE_S = 3.0


class PhoneUsePolicy:
    def __init__(self, confirmation_s=3.0, max_gap_s=2.5):
        self.confirmation_s = confirmation_s
        self.max_gap_s = max_gap_s
        self.reset()

    def reset(self):
        self.started_at = None
        self.last_sample_at = None
        self.last_seq = None
        self.last_phone_at = None
        self.misses = 0

    def interrupt_overlap(self):
        # Missing hand breaks use evidence, not the independent phone sighting.
        self.started_at = None

    def observe(self, result, frame_seq, sampled_at, completed_at):
        """Only distinct, recent input images can establish continuous overlap."""
        if (not isinstance(result, dict) or result.get('error') or
                not isinstance(result.get('phone_detected'), bool) or
                (result.get('hand_overlap') is not None and
                 not isinstance(result.get('hand_overlap'), bool))):
            self.reset()
            raise ValueError('Invalid phone detector result')
        if (isinstance(frame_seq, bool) or not isinstance(frame_seq, int) or
                not all(isinstance(value, (int, float)) and not isinstance(value, bool)
                        and math.isfinite(value) for value in (sampled_at, completed_at)) or
                completed_at < sampled_at or completed_at - sampled_at > PHONE_MAX_AGE_S):
            self.reset()
            raise ValueError('Phone detector input is expired')
        if (self.last_seq is not None and
                (frame_seq <= self.last_seq or sampled_at <= self.last_sample_at)):
            self.reset()
            raise ValueError('Phone detector input is repeated or out of order')

        continuous = (self.last_sample_at is not None and
                      sampled_at - self.last_sample_at <= self.max_gap_s)
        positive = result['phone_detected'] and result.get('hand_overlap') is True
        if positive:
            if self.started_at is None or not continuous:
                self.started_at = sampled_at
        else:
            self.started_at = None
        self.last_sample_at, self.last_seq = sampled_at, frame_seq
        if result['phone_detected']:
            self.last_phone_at = sampled_at
            self.misses = 0
        else:
            self.misses += 1
        seen_age = None if self.last_phone_at is None else max(0, int(round((completed_at-self.last_phone_at)*1000)))
        checking = (not result['phone_detected'] and seen_age is not None and seen_age < 2500 and self.misses < 2)
        sustained_ms = int(round((sampled_at - self.started_at) * 1000)) if positive else 0
        output = copy.deepcopy(result)
        output.update(status=('suspected' if sustained_ms >= self.confirmation_s * 1000 else 'detected')
                      if result['phone_detected'] else 'not_detected',
                      sustained_ms=sustained_ms, confirmation_ms=int(self.confirmation_s * 1000),
                      presence_state='checking' if checking else 'detected' if result['phone_detected'] else 'not_detected',
                      last_seen_age_ms=seen_age, absence_hold_ms=2500, consecutive_misses=self.misses)
        return output
