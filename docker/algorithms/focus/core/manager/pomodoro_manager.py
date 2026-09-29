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

        self.window_time_start = time.time()
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

    def reset_window(self):
        self.window_time_start = time.time()
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

    def foward_window(self):
        self.window_time_start = time.time()
        self.engage_level_queue = []
        self.engage_level_count = {
            EngageLevel.HighlyEngaged: 0,
            EngageLevel.Engaged: 0,
            EngageLevel.BarelyEngaged: 0,
            EngageLevel.NotEngaged: 0,
        }

    def get_window_time_elapsed(self):
        return time.time() - self.window_time_start

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

            self.logger.info(f'Focus level: {self.focus_level.name} ({self.focus_level.value}), score={focus_score:.4f}')
        except Exception as e:
            self.logger.error(f'Error calculating focus level: {e}')

    def update(self, engage_level: int):
        try:
            self.engage_level_queue.append(engage_level)
            self.engage_level_count[EngageLevel(engage_level)] += 1

            if self.get_window_time_elapsed() >= self.window_time_duration:
                self.calculate_focus_level()
                self.foward_window()
        except Exception as e:
            self.logger.error(f'Error updating pomodoro manager: {e}')
            self.reset_window()

    def full_reset(self):
        self.reset_window()
