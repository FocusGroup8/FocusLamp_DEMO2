"""
UI 颜色映射

集中管理 main.py 显示用到的所有 BGR 颜色字典。
- EMOTION_COLORS: 8 种情绪 → BGR 颜色
- GESTURE_COLORS: 7 种手势 → BGR 颜色
- COMBO_GESTURE_COLORS: 4 种组合手势 → BGR 颜色
- EMOTION_COMBO_COLORS: 2 种情绪+手势联动 → BGR 颜色

注：颈部保健操动作颜色 NECK_ACTION_COLORS 仍定义在 detectors/pose_detector/neck_exercise.py
    （与 ACTION_THRESHOLDS 配套使用，属于 detector 内部配置）
"""

# 情绪 -> 颜色 (BGR)
EMOTION_COLORS = {
    "Happiness": (0, 255, 0),      # 绿色
    "Sadness": (255, 0, 0),        # 蓝色
    "Anger": (0, 0, 255),          # 红色
    "Fear": (0, 255, 255),         # 黄色
    "Surprise": (255, 0, 255),     # 紫色
    "Disgust": (0, 128, 128),      # 深黄
    "Contempt": (128, 128, 0),     # 深蓝绿
    "Neutral": (200, 200, 200),    # 灰色
}

# 手势 -> 颜色 (BGR)
GESTURE_COLORS = {
    "Closed_Fist": (0, 0, 255),    # 红色
    "Open_Palm": (0, 255, 0),       # 绿色
    "Pointing_Up": (255, 0, 0),     # 蓝色
    "Thumb_Down": (0, 128, 255),    # 橙色
    "Thumb_Up": (0, 255, 128),      # 青绿
    "Victory": (255, 255, 0),       # 青色
    "ILoveYou": (255, 0, 255),      # 紫色
}

# 组合手势 -> 颜色 (BGR)
COMBO_GESTURE_COLORS = {
    "Open": (0, 255, 0),           # 绿色
    "Close": (0, 0, 255),          # 红色
    "VolumeUp": (0, 200, 255),     # 橙色
    "VolumeDown": (255, 100, 0),   # 蓝色
}

# 情绪+手势联动 -> 颜色 (BGR)
EMOTION_COMBO_COLORS = {
    "Welcome": (0, 255, 200),      # 青绿
    "Good": (0, 255, 128),         # 亮绿
}


def get_emotion_color(emotion: str) -> tuple:
    """根据情绪名称返回 BGR 颜色，未知情绪返回白色"""
    return EMOTION_COLORS.get(emotion, (255, 255, 255))


def get_gesture_color(gesture: str) -> tuple:
    """根据手势名称返回 BGR 颜色，未知手势返回白色"""
    return GESTURE_COLORS.get(gesture, (255, 255, 255))
