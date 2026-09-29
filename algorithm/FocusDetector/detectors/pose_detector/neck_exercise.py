"""
颈部保健操检测器

基于 MediaPipe Pose 的 33 个关键点，识别颈部保健操动作并评分。

支持动作：
  - 左右侧屈（Lateral Flexion）：头部向左右倾斜
  - 前后屈伸（Flexion/Extension）：头部前低/后仰
  - 左右旋转（Rotation）：头部向左右转动

评分维度：
  - 幅度评分：动作角度是否达到标准范围
  - 保持时间评分：最大角度处是否保持足够时间

关键点索引（MediaPipe Pose 33 点）：
  0  = 鼻尖 (nose)
  7  = 右耳 (right ear)
  8  = 左耳 (left ear)
  11 = 左肩 (left shoulder)
  12 = 右肩 (right shoulder)
"""

import time
import math
from dataclasses import dataclass, field

from utils.logger import setup_logging

TAG = __name__


@dataclass
class NeckAction:
    """单个颈部动作的检测结果"""
    name: str = "None"           # 动作名称
    angle: float = 0.0          # 当前角度（度）
    amplitude_score: float = 0.0  # 幅度评分 (0-100)
    hold_score: float = 0.0     # 保持时间评分 (0-100)
    total_score: float = 0.0    # 综合评分 (0-100)
    hold_duration: float = 0.0  # 已保持时间（秒）


@dataclass
class ActionThreshold:
    """动作阈值配置"""
    name: str = ""
    min_angle: float = 15.0      # 最低识别角度
    full_angle: float = 30.0     # 满分角度
    hold_target: float = 3.0     # 目标保持时间（秒）


# 动作阈值定义
ACTION_THRESHOLDS = {
    "Left_Lateral_Flexion":  ActionThreshold("Left_Lateral_Flexion",  min_angle=20, full_angle=35, hold_target=3.0),
    "Right_Lateral_Flexion": ActionThreshold("Right_Lateral_Flexion", min_angle=20, full_angle=35, hold_target=3.0),
    "Flexion":               ActionThreshold("Flexion",               min_angle=15, full_angle=30, hold_target=3.0),
    "Extension":             ActionThreshold("Extension",             min_angle=10, full_angle=20, hold_target=3.0),
    "Left_Rotation":         ActionThreshold("Left_Rotation",         min_angle=15, full_angle=35, hold_target=3.0),
    "Right_Rotation":        ActionThreshold("Right_Rotation",        min_angle=15, full_angle=35, hold_target=3.0),
}

# 动作 -> 颜色 (BGR)
NECK_ACTION_COLORS = {
    "Left_Lateral_Flexion":  (255, 180, 0),   # 蓝色
    "Right_Lateral_Flexion": (0, 180, 255),   # 橙色
    "Flexion":               (0, 255, 0),     # 绿色
    "Extension":             (0, 0, 255),     # 红色
    "Left_Rotation":         (255, 0, 180),   # 紫色
    "Right_Rotation":        (180, 0, 255),   # 粉色
    "None":                  (200, 200, 200), # 灰色
}


class NeckExerciseDetector:
    def __init__(self, visibility_threshold: float = 0.5):
        self.logger = setup_logging()
        self.visibility_threshold = visibility_threshold

        # 当前动作状态
        self._current_action = "None"
        self._current_angle = 0.0
        self._hold_start_time: float | None = None
        self._peak_angle = 0.0  # 当前动作的峰值角度

    def _get_landmark(self, pose_landmarks, idx: int) -> tuple | None:
        """获取关键点坐标，可见度不足返回 None"""
        if idx >= len(pose_landmarks):
            return None
        x, y, _z, vis = pose_landmarks[idx]
        if vis < self.visibility_threshold:
            return None
        return (x, y)

    def _calc_lateral_flexion_angle(self, pose_landmarks) -> tuple[float, str]:
        """
        计算左右侧屈角度。
        返回 (角度, 动作名)。

        侧屈判断：鼻尖水平偏移 + dy 明显减小（头整体倾斜）。
        旋转时 dy 基本不变，侧屈时 dy 明显减小。
        """
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)

        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"

        # 双肩中点
        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2

        # 鼻尖相对双肩中点的偏移
        dx = nose[0] - mid_x
        dy = mid_y - nose[1]  # y 轴向下为正

        # 前屈优先：当鼻尖接近或低于肩膀水平线时，优先判定为前屈
        if dy < 0.08:
            return 0.0, "None"

        angle = math.degrees(math.atan2(abs(dx), dy))

        # 侧屈区分：dy 相对肩宽的比值明显减小才判定为侧屈
        # 正常站立时 dy/shoulder_width 约 0.5-0.7，侧屈时 < 0.45
        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"

        dy_ratio = dy / shoulder_width
        if dy_ratio >= 0.45:
            # dy 没有明显减小，水平偏移来自旋转，不判定为侧屈
            return 0.0, "None"

        # 镜像翻转后：dx < 0 对应画面中的左侧屈
        if dx < 0:
            return angle, "Left_Lateral_Flexion"
        else:
            return angle, "Right_Lateral_Flexion"

    def _calc_flexion_extension_angle(self, pose_landmarks) -> tuple[float, str]:
        """
        计算前后屈伸角度。
        返回 (角度, 动作名)，前屈为 Flexion，后仰为 Extension。

        判断逻辑：
        - 正常站立时鼻尖到肩膀中点的垂直距离 dy 约 0.15~0.25
        - 低头时 dy 减小，当 dy < 0.08 时判定为前屈
        - 后仰时鼻尖到肩膀中点距离增大，用距离比判断
        """
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)

        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"

        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2

        # 鼻尖相对双肩中点的垂直偏移
        dy = mid_y - nose[1]  # 正值 = 鼻尖在肩膀上方，负值 = 鼻尖在肩膀下方

        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"

        # 前屈判断：dy < 0.08 说明鼻尖下移到接近肩膀水平线
        if dy < 0.08:
            # 前屈角度：dy 越小（甚至为负），角度越大
            # 正常 dy 约 0.20，前屈时 dy 接近 0 或为负
            flexion_angle = max(0, (0.20 - dy) / 0.20 * 45)  # 映射到 0~45 度
            return flexion_angle, "Flexion"

        # 后仰判断：用鼻尖与双肩中点的距离比
        nose_to_mid = math.dist(nose, (mid_x, mid_y))
        ratio = nose_to_mid / shoulder_width
        # 正常 ratio 约 0.5-0.6，后仰时 > 0.85
        if ratio > 0.85:
            angle = (ratio - 0.75) * 200
            return min(angle, 45), "Extension"

        return 0.0, "None"

    def _calc_rotation_angle(self, pose_landmarks) -> tuple[float, str]:
        """
        计算左右旋转角度。
        返回 (角度, 动作名)。

        旋转判断：鼻尖水平偏移 + dy 基本不变（头绕纵轴旋转）。
        用鼻尖水平偏移角度作为旋转角度，通过 dy_ratio 区分旋转和侧屈。
        旋转时 dy_ratio >= 0.45，侧屈时 dy_ratio < 0.45。
        """
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)
        l_ear = self._get_landmark(pose_landmarks, 7)   # 右耳
        r_ear = self._get_landmark(pose_landmarks, 8)   # 左耳

        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"

        # 双肩中点
        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2

        # 鼻尖相对双肩中点的偏移
        dx = nose[0] - mid_x
        dy = mid_y - nose[1]  # y 轴向下为正

        # 前屈优先
        if dy < 0.08:
            return 0.0, "None"

        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"

        # 旋转区分：dy_ratio >= 0.45 说明 dy 没明显减小，水平偏移来自旋转
        dy_ratio = dy / shoulder_width
        if dy_ratio < 0.45:
            # dy 明显减小，是侧屈不是旋转
            return 0.0, "None"

        # 用鼻尖水平偏移角度作为旋转角度
        angle = math.degrees(math.atan2(abs(dx), dy))

        if angle < 5:  # 最小角度阈值
            return 0.0, "None"

        # 方向判定：用耳-鼻距离差确定旋转方向
        # 转向哪侧，哪侧耳离鼻尖更近
        if l_ear and r_ear:
            dist_left_ear = math.dist(l_ear, nose)
            dist_right_ear = math.dist(r_ear, nose)
            diff = dist_left_ear - dist_right_ear
            # 镜像翻转后方向对调
            if diff > 0:
                return angle, "Left_Rotation"
            else:
                return angle, "Right_Rotation"

        # 无耳朵数据时用 dx 方向
        if dx < 0:
            return angle, "Left_Rotation"
        else:
            return angle, "Right_Rotation"

    def update(self, pose_landmarks: list | None) -> NeckAction:
        """
        更新颈部动作检测，返回当前动作状态和评分。

        Args:
            pose_landmarks: MediaPipe Pose 的 33 个关键点列表，每个元素为 (x, y, z, visibility)

        Returns:
            NeckAction: 当前动作的检测结果
        """
        if pose_landmarks is None:
            return self._reset_action()

        # 依次检测各类动作，取角度最大的作为当前动作
        candidates = [
            self._calc_flexion_extension_angle(pose_landmarks),
            self._calc_lateral_flexion_angle(pose_landmarks),
            self._calc_rotation_angle(pose_landmarks),
        ]

        # 找到角度最大的有效动作
        best_angle = 0.0
        best_action = "None"
        for angle, action_name in candidates:
            threshold = ACTION_THRESHOLDS.get(action_name)
            if threshold and angle >= threshold.min_angle:
                if angle > best_angle:
                    best_angle = angle
                    best_action = action_name

        # 动作切换时重置保持时间
        if best_action != self._current_action:
            self._current_action = best_action
            self._hold_start_time = time.time() if best_action != "None" else None
            self._peak_angle = best_angle

        # 更新峰值角度
        if best_action != "None" and best_angle > self._peak_angle:
            self._peak_angle = best_angle

        self._current_angle = best_angle

        # 计算评分
        if best_action == "None":
            return NeckAction(name="None")

        threshold = ACTION_THRESHOLDS[best_action]

        # 幅度评分：线性映射 [min_angle, full_angle] -> [0, 100]
        amplitude_score = min(100, max(0,
            (self._peak_angle - threshold.min_angle) / (threshold.full_angle - threshold.min_angle) * 100
        ))

        # 保持时间评分
        hold_duration = 0.0
        hold_score = 0.0
        if self._hold_start_time is not None:
            hold_duration = time.time() - self._hold_start_time
            hold_score = min(100, (hold_duration / threshold.hold_target) * 100)

        # 综合评分：幅度 60% + 保持时间 40%
        total_score = amplitude_score * 0.6 + hold_score * 0.4

        return NeckAction(
            name=best_action,
            angle=self._peak_angle,
            amplitude_score=round(amplitude_score, 1),
            hold_score=round(hold_score, 1),
            total_score=round(total_score, 1),
            hold_duration=round(hold_duration, 1),
        )

    def _reset_action(self) -> NeckAction:
        """重置动作状态"""
        self._current_action = "None"
        self._current_angle = 0.0
        self._hold_start_time = None
        self._peak_angle = 0.0
        return NeckAction(name="None")
