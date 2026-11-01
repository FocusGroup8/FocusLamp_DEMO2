"""Transport-only focus metadata. Does not modify classification or score."""
import hashlib
import time
import uuid

def canonical_engagement(level):
    if not isinstance(level, str):
        return 'Unknown'
    key = level.replace('-', '').replace('_', '').replace(' ', '').casefold()
    return {'highlyengaged': 'HighlyEngaged', 'engaged': 'Engaged',
            'barelyengaged': 'BarelyEngaged', 'notengaged': 'NotEngaged'}.get(key, 'Unknown')

def clock_residence_ms(received_at, now):
    """Only local monotonic residence, subtracted by the clock's owner.

    Round down: any remainder stays in the conservative network-age budget.
    """
    return min(0xffffffff, max(0, int((now-received_at)*1000)))


class FocusMetadata:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.boot = uuid.uuid4().hex
        self.seq = 0
        self.latest = None
        self.source_generation = None
        self.provider_epoch = None

    def commit(self, response, frame_seq, received_at, generation):
        now = self.clock()
        result = (response or {}).get('result', {})
        sampling = result.get('sampling', {})
        engage = result.get('engage_detect_result', {})
        level = canonical_engagement(engage.get('engage_level_name', result.get('engage_level_name', 'Unknown')))
        inference = sampling.get('inference_seq', 0)
        age = sampling.get('classification_age_ms')
        reported_epoch = sampling.get('source_epoch')
        has_epoch = isinstance(reported_epoch, str) and bool(reported_epoch)
        if generation != self.source_generation:
            self.source_generation = generation
            self.provider_epoch = None
        if has_epoch:
            self.provider_epoch = reported_epoch
        # A failed request is an invalid sample from the current source, not
        # evidence that the detector restarted. Retain its namespace until a
        # real provider epoch or camera generation changes it.
        epoch = self.provider_epoch or 'unsupported'
        source = hashlib.sha256(f'{self.boot}:{generation}:{epoch}'.encode()).hexdigest()
        if self.latest and self.latest['source_epoch'] == source and frame_seq <= self.latest['frame_seq']:
            return  # duplicate/out-of-order input cannot renew expiry or validity
        valid = (has_epoch and sampling.get('state') == 'ready' and isinstance(age, (int, float))
                 and age >= 0 and isinstance(inference, int) and inference > 0
                 and level in ('HighlyEngaged', 'Engaged', 'BarelyEngaged', 'NotEngaged'))
        if not has_epoch and self.latest and self.latest['source_epoch'] == source:
            inference = self.latest['inference_seq']
        self.seq += 1
        # Includes dispatch waiting, inference and HTTP round trip, conservatively.
        transfer = max(0, int((now-received_at)*1000))
        self.latest = {
            'source_epoch': source,
            'sample_seq': self.seq, 'frame_seq': frame_seq,
            'inference_seq': max(0, inference) if isinstance(inference, int) else 0,
            'valid': valid, 'engage_level_name': level if valid else 'Unknown',
            'focus_level_name': result.get('focus_level_name', 'Pending'),
            'focus_score': result.get('focus_score'),
            'age_ms': max(0, int(age or 0))+transfer,
            'sample_age_ms': transfer, '_at': now,
        }

    def payload(self):
        if self.latest is None:
            return None
        result = dict(self.latest)
        wait = max(0, int((self.clock()-result.pop('_at'))*1000))
        # MCU measures transport using its own echoed monotonic clock.
        result['age_ms'] = min(0xffffffff, result['age_ms']+wait)
        result['sample_age_ms'] = min(0xffffffff, result['sample_age_ms']+wait)
        return result
