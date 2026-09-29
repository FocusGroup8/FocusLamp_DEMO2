"""
UI 关键点绘制

封装手部/肢体关键点和骨骼连接线的绘制逻辑。
- HAND_CONNECTIONS: MediaPipe 21 个手部关键点连接关系
- POSE_CONNECTIONS: MediaPipe 33 个肢体关键点连接关系
- draw_hand_landmarks(): 在画面上绘制手部关键点
- draw_pose_landmarks(): 在画面上绘制肢体关键点（过滤低可见度）
"""

import cv2

# MediaPipe 手部关键点连接关系
HAND_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4),       # 拇指
    (0, 5), (5, 6), (6, 7), (7, 8),       # 食指
    (0, 9), (9, 10), (10, 11), (11, 12),  # 中指
    (0, 13), (13, 14), (14, 15), (15, 16),# 无名指
    (0, 17), (17, 18), (18, 19), (19, 20),# 小指
    (5, 9), (9, 13), (13, 17),            # 掌心横连
]

# MediaPipe 肢体关键点连接关系（33 个关键点）
POSE_CONNECTIONS = [
    # 面部
    (0, 1), (1, 2), (2, 3), (3, 7),       # 右眼
    (0, 4), (4, 5), (5, 6), (6, 8),       # 左眼
    (9, 10),                               # 嘴
    # 躯干
    (11, 12),                              # 肩膀
    (11, 23), (12, 24),                    # 肩→髋
    (23, 24),                              # 髋
    # 右臂
    (11, 13), (13, 15),                    # 右肩→右肘→右腕
    (15, 17), (15, 19), (15, 21),          # 右腕→右手
    (17, 19),                              # 右手小指→食指
    # 左臂
    (12, 14), (14, 16),                    # 左肩→左肘→左腕
    (16, 18), (16, 20), (16, 22),          # 左腕→左手
    (18, 20),                              # 左手小指→食指
    # 右腿
    (23, 25), (25, 27),                    # 右髋→右膝→右踝
    (27, 29), (27, 31), (29, 31),          # 右踝→右脚
    # 左腿
    (24, 26), (26, 28),                    # 左髋→左膝→左踝
    (28, 30), (28, 32), (30, 32),          # 左踝→左脚
]


def draw_hand_landmarks(frame, hand_landmarks_list):
    """在画面上绘制手部关键点和连接线

    Args:
        frame: OpenCV BGR 图像
        hand_landmarks_list: 每只手的关键点列表，每个元素为 [(x0, y0), ..., (x20, y20)]（归一化坐标）
    """
    h, w = frame.shape[:2]
    for hand_pts in hand_landmarks_list:
        # 将归一化坐标转换为像素坐标
        points = [(int(pt[0] * w), int(pt[1] * h)) for pt in hand_pts]

        # 绘制连接线
        for start_idx, end_idx in HAND_CONNECTIONS:
            if start_idx < len(points) and end_idx < len(points):
                cv2.line(frame, points[start_idx], points[end_idx], (255, 200, 0), 2)

        # 绘制关键点
        for i, (px, py) in enumerate(points):
            cv2.circle(frame, (px, py), 4, (0, 255, 255), -1)
            cv2.circle(frame, (px, py), 4, (0, 0, 0), 1)


def draw_pose_landmarks(frame, pose_landmarks, min_visibility: float = 0.5):
    """在画面上绘制肢体关键点和骨骼连接线

    Args:
        frame: OpenCV BGR 图像
        pose_landmarks: 33 个关键点列表，每个元素为 (x, y, z, visibility)
        min_visibility: 可见度低于此值的关键点不绘制
    """
    h, w = frame.shape[:2]
    # 将归一化坐标转换为像素坐标，过滤低可见度关键点
    points = []
    for lm in pose_landmarks:
        x, y, _z, vis = lm
        if vis >= min_visibility:
            points.append((int(x * w), int(y * h)))
        else:
            points.append(None)

    # 绘制连接线
    for start_idx, end_idx in POSE_CONNECTIONS:
        if start_idx < len(points) and end_idx < len(points):
            if points[start_idx] is not None and points[end_idx] is not None:
                cv2.line(frame, points[start_idx], points[end_idx], (0, 200, 255), 2)

    # 绘制关键点
    for pt in points:
        if pt is not None:
            cv2.circle(frame, pt, 3, (0, 255, 0), -1)
            cv2.circle(frame, pt, 3, (0, 0, 0), 1)
