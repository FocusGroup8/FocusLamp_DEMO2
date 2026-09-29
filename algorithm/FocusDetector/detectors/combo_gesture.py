import time
from utils.logger import setup_logging

TAG = __name__

# 组合手势定义：(起始手势, 结束手势) -> 组合名称
COMBO_GESTURES = {
    ("Closed_Fist", "Open_Palm"): "Open",
    ("Open_Palm", "Closed_Fist"): "Close",
    ("Closed_Fist", "Thumb_Up"): "VolumeUp",
    ("Closed_Fist", "Thumb_Down"): "VolumeDown",
}

# 情绪+手势联动定义：(情绪, 手势) -> 联动名称
EMOTION_GESTURE_COMBOS = {
    ("Happiness", "Open_Palm"): "Welcome",
    ("Happiness", "Thumb_Up"): "Good",
}

# 无效手势值（既忽略 Python None，也忽略 MediaPipe 返回的字符串 "None"）
_INVALID_GESTURES = {None, "None"}


class ComboGestureDetector:
    """基于事件驱动的组合手势检测器。

    核心思路：忽略 None 和 "None" 过渡帧，只关注有效手势之间的变化事件。
    当有效手势从 A 变为 B 时，检查 (A, B) 是否为已定义的组合手势。

    - None / "None" 被视为过渡噪声，不影响手势序列判断
    """

    def __init__(self):
        self.logger = setup_logging()

        self._last_valid_gesture: str | None = None     # 最近一次有效手势（忽略 None）

        # 调试：手势变化历史记录
        self._gesture_history: list[dict] = []
        self._program_start_time: float = time.time()

    def _log_event(self, event_type: str, detail: str):
        """记录手势变化事件"""
        elapsed = time.time() - self._program_start_time
        event = {
            "time": f"{elapsed:.2f}",
            "type": event_type,
            "detail": detail,
        }
        self._gesture_history.append(event)
        if len(self._gesture_history) > 100:
            self._gesture_history = self._gesture_history[-100:]
        self.logger.bind(tag=TAG).debug(f"[{elapsed:.2f}s] {event_type}: {detail}")

    def update(self, current_gesture: str | None) -> str | None:
        """更新当前手势，返回触发的组合手势名称或 None。"""
        # 忽略 None 和 "None"：过渡帧不影响判断
        if current_gesture in _INVALID_GESTURES:
            return None

        # 第一个有效手势
        if self._last_valid_gesture is None:
            self._last_valid_gesture = current_gesture
            self._log_event("FIRST_GESTURE", f"Detected first gesture: {current_gesture}")
            return None

        # 同一个手势持续中
        if current_gesture == self._last_valid_gesture:
            return None

        # 有效手势发生了变化
        prev_gesture = self._last_valid_gesture
        self._log_event("GESTURE_CHANGE", f"{prev_gesture} -> {current_gesture}")

        combo_key = (prev_gesture, current_gesture)
        combo_name = COMBO_GESTURES.get(combo_key)
        if combo_name:
            self._log_event("COMBO_TRIGGERED", f"{prev_gesture} -> {current_gesture} = {combo_name}")
            self.logger.bind(tag=TAG).info(
                f"Combo gesture triggered: {prev_gesture} -> {current_gesture} = {combo_name}"
            )
        else:
            self._log_event("NO_COMBO_MATCH", f"({prev_gesture}, {current_gesture}) not in COMBO_GESTURES")

        # 更新为当前手势
        self._last_valid_gesture = current_gesture
        return combo_name

    def get_state(self) -> dict:
        """获取当前状态，用于 UI 显示"""
        return {
            "last_valid_gesture": self._last_valid_gesture,
        }
