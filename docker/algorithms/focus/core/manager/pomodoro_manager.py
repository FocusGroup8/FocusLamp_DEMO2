import time
import numpy as np

from core.utils.logger import setup_logging
from core.types.focus_corr_state import FocusLevel, FocusLevelThreshold, FocusScoreWeight
from core.types.engage import EngageLevel

TAG = __name__

class PomodoroManager:
    def __init__(self, window_duration: int = 90):
        self.logger = setup_logging()
        self.logger.info('PomodoroManager initialized')

        self.window_time_start = None
        self.last_frame_at = None
        self.input_gap_limit_s = 1.0
        self.max_input_gap_s = 0.0
        self.window_invalid = False
        self.score_usable = True
        self.window_time_duration = window_duration

        self.engage_level_queue = []
        self.engage_level_count = {
            EngageLevel.HighlyEngaged: 0,
            EngageLevel.Engaged: 0,
            EngageLevel.BarelyEngaged: 0,
            EngageLevel.NotEngaged: 0,
        }

        self.engage_level_score_map = {
            EngageLevel.HighlyEngaged: 5,
            EngageLevel.Engaged: 3,
            EngageLevel.BarelyEngaged: 1,
            EngageLevel.NotEngaged: 0,
        }

        self.focus_level = FocusLevel.Neutral
        self.prev_focus_score = None
        self.last_focus_result = None
        self.completed_at_ms = None
        self.completed_at_monotonic = None

    def reset_window(self):
        self.window_time_start = None
        self.last_frame_at = None
        self.input_gap_limit_s = 1.0
        self.max_input_gap_s = 0.0
        self.window_invalid = False
        self.score_usable = True
        self.engage_level_queue = []
        self.engage_level_count = {
            EngageLevel.HighlyEngaged: 0,
            EngageLevel.Engaged: 0,
            EngageLevel.BarelyEngaged: 0,
            EngageLevel.NotEngaged: 0,
        }
        self.focus_level = FocusLevel.Neutral
        self.prev_focus_score = None
        self.last_focus_result = None
        self.completed_at_ms = None
        self.completed_at_monotonic = None

    def note_frame(self):
        """An input/processing gap is missing evidence, never NotEngaged."""
        now = time.monotonic()
        if self.last_frame_at is not None and self.window_time_start is not None:
            gap = max(0.0, now - self.last_frame_at)
            self.max_input_gap_s = max(self.max_input_gap_s, gap)
            if gap > self.input_gap_limit_s:
                self.window_invalid = True
                self.score_usable = False
        self.last_frame_at = now
        # Do not depend on a new model classification to finish an invalid window.
        if self.window_invalid and self.get_window_time_elapsed() >= self.window_time_duration:
            self.foward_window()

    def foward_window(self):
        self.window_time_start = time.monotonic()
        self.window_invalid = False
        self.engage_level_queue = []
        self.engage_level_count = {
            EngageLevel.HighlyEngaged: 0,
            EngageLevel.Engaged: 0,
            EngageLevel.BarelyEngaged: 0,
            EngageLevel.NotEngaged: 0,
        }

    def get_window_time_elapsed(self):
        return 0.0 if self.window_time_start is None else max(0.0, time.monotonic() - self.window_time_start)

    def window_metadata(self):
        """Presentation metadata only; the weighted-log score stays unchanged."""
        return {
            'state': 'insufficient_data' if not self.score_usable else 'ready' if self.last_focus_result is not None else 'waiting' if self.window_time_start is None else 'collecting',
            'score_usable': self.score_usable,
            'input_gap_limit_s': self.input_gap_limit_s,
            'max_input_gap_s': round(self.max_input_gap_s, 3),
            'duration_s': self.window_time_duration,
            'elapsed_s': min(self.window_time_duration, self.get_window_time_elapsed()),
            'sample_count': len(self.engage_level_queue),
            'completed_at_ms': self.completed_at_ms,
            'completed_age_ms': None if self.completed_at_monotonic is None else max(0, int((time.monotonic() - self.completed_at_monotonic) * 1000)),
        }

    def get_focus_score(self):
        try:
            focus_score = 0
            for level, count in self.engage_level_count.items():
                focus_score += self.engage_level_score_map[level] * count
            return np.log(focus_score + 1)
        except Exception as e:
            self.logger.error(f'Error getting focus score: {e}')
            return 0

    def calculate_focus_level(self):
        if self.window_invalid:
            return
        try:
            focus_score = self.get_focus_score()

            if self.prev_focus_score is not None:
                focus_score = FocusScoreWeight.PrevWindowWeight.value * self.prev_focus_score + FocusScoreWeight.CurrentWindowWeight.value * focus_score

            self.prev_focus_score = focus_score

            if focus_score > FocusLevelThreshold.HighFocused.value:
                self.focus_level = FocusLevel.HighFocused
            elif focus_score > FocusLevelThreshold.MediumFocused.value:
                self.focus_level = FocusLevel.MediumFocused
            elif focus_score > FocusLevelThreshold.LowFocused.value:
                self.focus_level = FocusLevel.LowFocused
            elif focus_score > FocusLevelThreshold.LowDistracted.value:
                self.focus_level = FocusLevel.Neutral
            elif focus_score > FocusLevelThreshold.MediumDistracted.value:
                self.focus_level = FocusLevel.LowDistracted
            elif focus_score > FocusLevelThreshold.HighDistracted.value:
                self.focus_level = FocusLevel.MediumDistracted
            else:
                self.focus_level = FocusLevel.HighDistracted

            self.last_focus_result = {
                'focus_level': self.focus_level.value,
                'focus_level_name': self.focus_level.name,
                'focus_score': round(float(focus_score), 4),
                'engage_level_count': {k.value: v for k, v in self.engage_level_count.items()},
            }
            self.score_usable = True
            self.completed_at_ms = int(time.time() * 1000)
            self.completed_at_monotonic = time.monotonic()

            self.logger.info(f'Focus level: {self.focus_level.name} ({self.focus_level.value}), score={focus_score:.4f}')
        except Exception as e:
            self.logger.error(f'Error calculating focus level: {e}')

    def update(self, engage_level: int):
        try:
            level = EngageLevel(engage_level)
            if self.window_time_start is None:
                self.window_time_start = time.monotonic()
            self.engage_level_queue.append(engage_level)
            self.engage_level_count[level] += 1

            if self.get_window_time_elapsed() >= self.window_time_duration:
                self.calculate_focus_level()
                self.foward_window()
        except Exception as e:
            self.logger.error(f'Error updating pomodoro manager: {e}')
            self.reset_window()

    def full_reset(self):
        self.reset_window()
