"""
UI 信息覆盖层

封装 main.py 中的双列调试信息显示逻辑。

布局：
  左侧主信息列 (x=10)：
    - Emotion  (y=30)
    - Gesture  (y=60)
    - Combo     (y=90)
    - Pose      (y=120)
    - Neck      (y=150)
    - Face XYD  (y=185)  ← x_cm / y_cm / distance_cm

  右侧辅助信息列 (x=screen_w - 280)：
    - EmCombo    (y=30)
    - NeckScore  (y=55)  ← 仅 Neck 动作非 None 时显示
    - Hold       (y=78)  ← 仅 Neck 动作非 None 时显示
"""

from typing import TYPE_CHECKING, Optional

import cv2

if TYPE_CHECKING:
    # 仅用于类型注解，运行时不导入（避免 ui 模块依赖 detectors）
    from detectors.pose_detector.neck_exercise import NeckAction
    from detectors.face_position_estimator.base import FacePosition


def draw_overlay(
    display_frame,
    *,
    emotion_text: str = "No face",
    emotion_color: tuple = (200, 200, 200),
    gesture_text: str = "No gesture",
    gesture_color: tuple = (200, 200, 200),
    combo_text: str = "None",
    combo_color: tuple = (200, 200, 200),
    pose_detected: bool = False,
    neck_action: Optional["NeckAction"] = None,
    neck_color: tuple = (200, 200, 200),
    face_pos: Optional["FacePosition"] = None,
    emotion_combo_text: str = "None",
    emotion_combo_color: tuple = (200, 200, 200),
    fps: Optional[float] = None,
) -> None:
    """在 display_frame 上绘制双列信息覆盖层（原地修改）

    Args:
        display_frame: OpenCV BGR 图像（会被原地修改）
        emotion_text, emotion_color: 情绪文本与颜色
        gesture_text, gesture_color: 手势文本与颜色
        combo_text, combo_color: 组合手势文本与颜色
        pose_detected: 是否检测到肢体
        neck_action: NeckAction 对象或 None（None 时显示 "None"）
        neck_color: 颈部动作对应的 BGR 颜色（由 NECK_ACTION_COLORS 查表得到）
        face_pos: FacePosition 对象或 None
        emotion_combo_text, emotion_combo_color: 情绪+手势联动文本与颜色
        fps: 帧率（可选），不为 None 时在左下角显示
    """
    screen_h, screen_w = display_frame.shape[:2]
    right_x = screen_w - 280   # 右侧辅助信息列起始 x

    neck_name = neck_action.name if neck_action is not None else "None"

    # ===== 左侧主信息列（紧凑排列）=====
    cv2.putText(display_frame, f"Emotion: {emotion_text}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, emotion_color, 2)
    cv2.putText(display_frame, f"Gesture: {gesture_text}", (10, 60),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, gesture_color, 2)
    cv2.putText(display_frame, f"Combo: {combo_text}", (10, 90),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, combo_color, 2)
    cv2.putText(display_frame,
                f"Pose: {'Detected' if pose_detected else 'No pose'}",
                (10, 120), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 200, 255), 2)
    cv2.putText(display_frame, f"Neck: {neck_name}", (10, 150),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, neck_color, 2)
    if face_pos is not None:
        face_color = (0, 255, 255) if face_pos.valid else (100, 100, 100)
        cv2.putText(display_frame,
                    f"Face XYD: ({face_pos.x_cm:+.1f}, {face_pos.y_cm:+.1f}, {face_pos.distance_cm:.1f}) cm",
                    (10, 185), cv2.FONT_HERSHEY_SIMPLEX, 0.65, face_color, 2)

    # ===== 右侧辅助信息列 =====
    cv2.putText(display_frame, f"EmCombo: {emotion_combo_text}", (right_x, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, emotion_combo_color, 1)
    if neck_action is not None and neck_name != "None":
        cv2.putText(display_frame,
                    f"NeckScore: {neck_action.total_score:.0f} (A:{neck_action.amplitude_score:.0f} H:{neck_action.hold_score:.0f})",
                    (right_x, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.5, neck_color, 1)
        cv2.putText(display_frame,
                    f"Hold: {neck_action.hold_duration:.1f}s  Angle: {neck_action.angle:.1f}deg",
                    (right_x, 78), cv2.FONT_HERSHEY_SIMPLEX, 0.5, neck_color, 1)

    # ===== 左下角 FPS 显示 =====
    if fps is not None:
        # FPS 颜色：>25 绿色 / 15-25 黄色 / <15 红色
        fps_color = (0, 255, 0) if fps > 25 else ((0, 255, 255) if fps > 15 else (0, 0, 255))
        cv2.putText(display_frame, f"FPS: {fps:.1f}", (10, screen_h - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, fps_color, 2)
