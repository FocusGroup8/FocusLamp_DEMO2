import os
import time
import math
import base64
import threading
from dataclasses import dataclass

import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
from mediapipe.framework.formats import landmark_pb2
from flask import Flask, request, jsonify
from PIL import Image, ImageDraw, ImageFont

app = Flask(__name__)

PORT = int(os.environ.get('PORT', 9010))

# MediaPipe solutions for drawing the pose skeleton
mp_pose = mp.solutions.pose
mp_drawing = mp.solutions.drawing_utils

# PoseLandmarker model path (same directory as server.py)
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pose_landmarker_full.task')

# PoseLandmarker initialization (synchronous IMAGE running mode)
_base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
_options = vision.PoseLandmarkerOptions(
    base_options=_base_options,
    running_mode=vision.RunningMode.IMAGE,
    num_poses=1,
    min_pose_detection_confidence=0.5,
    min_pose_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
pose_landmarker = vision.PoseLandmarker.create_from_options(_options)

# PoseLandmarker is not guaranteed thread-safe; serialize detect() calls
pose_lock = threading.Lock()


# ============ Neck Exercise Detector (authoritative FocusDetector implementation) ============

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

# 动作中文名（用于可视化）
NECK_ACTION_NAMES_CN = {
    "Left_Lateral_Flexion":  "左侧屈",
    "Right_Lateral_Flexion": "右侧屈",
    "Flexion":               "前屈",
    "Extension":             "后伸",
    "Left_Rotation":         "左旋转",
    "Right_Rotation":        "右旋转",
    "None":                  "无",
}


class NeckExerciseDetector:
    def __init__(self, visibility_threshold: float = 0.5):
        self.visibility_threshold = visibility_threshold
        self._current_action = "None"
        self._current_angle = 0.0
        self._hold_start_time = None
        self._peak_angle = 0.0

    def _get_landmark(self, pose_landmarks, idx):
        if idx >= len(pose_landmarks):
            return None
        x, y, _z, vis = pose_landmarks[idx]
        if vis < self.visibility_threshold:
            return None
        return (x, y)

    def _calc_lateral_flexion_angle(self, pose_landmarks):
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)
        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"
        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2
        dx = nose[0] - mid_x
        dy = mid_y - nose[1]
        if dy < 0.08:
            return 0.0, "None"
        angle = math.degrees(math.atan2(abs(dx), dy))
        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"
        dy_ratio = dy / shoulder_width
        if dy_ratio >= 0.45:
            return 0.0, "None"
        if dx < 0:
            return angle, "Left_Lateral_Flexion"
        else:
            return angle, "Right_Lateral_Flexion"

    def _calc_flexion_extension_angle(self, pose_landmarks):
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)
        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"
        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2
        dy = mid_y - nose[1]
        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"
        if dy < 0.08:
            flexion_angle = max(0, (0.20 - dy) / 0.20 * 45)
            return flexion_angle, "Flexion"
        nose_to_mid = math.dist(nose, (mid_x, mid_y))
        ratio = nose_to_mid / shoulder_width
        if ratio > 0.85:
            angle = (ratio - 0.75) * 200
            return min(angle, 45), "Extension"
        return 0.0, "None"

    def _calc_rotation_angle(self, pose_landmarks):
        nose = self._get_landmark(pose_landmarks, 0)
        l_shoulder = self._get_landmark(pose_landmarks, 11)
        r_shoulder = self._get_landmark(pose_landmarks, 12)
        l_ear = self._get_landmark(pose_landmarks, 7)
        r_ear = self._get_landmark(pose_landmarks, 8)
        if not all([nose, l_shoulder, r_shoulder]):
            return 0.0, "None"
        mid_x = (l_shoulder[0] + r_shoulder[0]) / 2
        mid_y = (l_shoulder[1] + r_shoulder[1]) / 2
        dx = nose[0] - mid_x
        dy = mid_y - nose[1]
        if dy < 0.08:
            return 0.0, "None"
        shoulder_width = math.dist(l_shoulder, r_shoulder)
        if shoulder_width < 0.001:
            return 0.0, "None"
        dy_ratio = dy / shoulder_width
        if dy_ratio < 0.45:
            return 0.0, "None"
        angle = math.degrees(math.atan2(abs(dx), dy))
        if angle < 5:
            return 0.0, "None"
        if l_ear and r_ear:
            dist_left_ear = math.dist(l_ear, nose)
            dist_right_ear = math.dist(r_ear, nose)
            diff = dist_left_ear - dist_right_ear
            if diff > 0:
                return angle, "Left_Rotation"
            else:
                return angle, "Right_Rotation"
        if dx < 0:
            return angle, "Left_Rotation"
        else:
            return angle, "Right_Rotation"

    def update(self, pose_landmarks):
        if pose_landmarks is None:
            return self._reset_action()
        candidates = [
            self._calc_flexion_extension_angle(pose_landmarks),
            self._calc_lateral_flexion_angle(pose_landmarks),
            self._calc_rotation_angle(pose_landmarks),
        ]
        best_angle = 0.0
        best_action = "None"
        for angle, action_name in candidates:
            threshold = ACTION_THRESHOLDS.get(action_name)
            if threshold and angle >= threshold.min_angle:
                if angle > best_angle:
                    best_angle = angle
                    best_action = action_name
        if best_action != self._current_action:
            self._current_action = best_action
            self._hold_start_time = time.time() if best_action != "None" else None
            self._peak_angle = best_angle
        if best_action != "None" and best_angle > self._peak_angle:
            self._peak_angle = best_angle
        self._current_angle = best_angle
        if best_action == "None":
            return NeckAction(name="None")
        threshold = ACTION_THRESHOLDS[best_action]
        amplitude_score = min(100, max(0,
            (self._peak_angle - threshold.min_angle) / (threshold.full_angle - threshold.min_angle) * 100
        ))
        hold_duration = 0.0
        hold_score = 0.0
        if self._hold_start_time is not None:
            hold_duration = time.time() - self._hold_start_time
            hold_score = min(100, (hold_duration / threshold.hold_target) * 100)
        total_score = amplitude_score * 0.6 + hold_score * 0.4
        return NeckAction(
            name=best_action,
            angle=self._peak_angle,
            amplitude_score=round(amplitude_score, 1),
            hold_score=round(hold_score, 1),
            total_score=round(total_score, 1),
            hold_duration=round(hold_duration, 1),
        )

    def _reset_action(self):
        self._current_action = "None"
        self._current_angle = 0.0
        self._hold_start_time = None
        self._peak_angle = 0.0
        return NeckAction(name="None")

    def reset(self):
        """Public reset method"""
        self._reset_action()


# Global stateful neck detector instance (tracks hold time across frames)
neck_detector = NeckExerciseDetector()


# ============ PIL Chinese text rendering ============

_font_cache = {}


def _get_font(size=24):
    """获取支持中文的字体（Dockerfile 已安装 fonts-wqy-microhei）"""
    if size in _font_cache:
        return _font_cache[size]
    font_paths = [
        '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    ]
    for fp in font_paths:
        if os.path.exists(fp):
            try:
                font = ImageFont.truetype(fp, size)
                _font_cache[size] = font
                return font
            except Exception:
                pass
    font = ImageFont.load_default()
    _font_cache[size] = font
    return font


def put_chinese_text(img, text, pos, color=(255, 255, 255), size=24):
    """在 OpenCV 图像上绘制中文文字"""
    pil_img = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(pil_img)
    font = _get_font(size)
    draw.text(pos, text, font=font, fill=color[::-1])  # BGR->RGB
    return cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)


# ============ Image decoding & pose detection ============

def decode_image(image_base64):
    """解码 base64 图像"""
    img_bytes = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_bytes, np.uint8)
    return cv2.imdecode(nparr, cv2.IMREAD_COLOR)


def _run_pose_landmarker(image_bgr):
    """运行 PoseLandmarker（同步 IMAGE 模式）。

    Args:
        image_bgr: BGR 格式的 numpy 图像（应已水平翻转）。

    Returns:
        PoseLandmarkerResult，其中 result.pose_landmarks 为
        list[list[NormalizedLandmark]]，每个 NormalizedLandmark 含
        .x/.y/.z/.visibility（x/y 归一化到 [0,1]）。
    """
    rgb = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2RGB)
    mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
    with pose_lock:
        result = pose_landmarker.detect(mp_image)
    return result


def _neck_action_to_dict(action: NeckAction) -> dict:
    """将 NeckAction 转换为 JSON 可序列化的字典"""
    return {
        'name': action.name,
        'angle': round(action.angle, 1),
        'amplitude_score': action.amplitude_score,
        'hold_score': action.hold_score,
        'total_score': action.total_score,
        'hold_duration': action.hold_duration,
    }


def detect_pose(image):
    """检测姿态并运行颈部动作检测器。

    输入图像会先水平翻转（镜像），再用 PoseLandmarker（IMAGE 模式）检测。
    返回 (result_dict, pose_landmarker_result)，后者用于可视化绘制骨骼，
    避免在 /detect_visualize 中重复运行检测。
    """
    # 水平翻转（镜像效果）
    flipped = cv2.flip(image, 1)

    try:
        landmarker_result = _run_pose_landmarker(flipped)
    except Exception as e:
        print(f"[WARN] PoseLandmarker.detect failed: {e}", flush=True)
        neck_action = neck_detector.update(None)
        return {
            'pose_detected': False,
            'pose_landmarks': [],
            'neck_action': _neck_action_to_dict(neck_action),
        }, None

    if not landmarker_result.pose_landmarks:
        # 未检测到姿态 - 重置颈部动作状态
        neck_action = neck_detector.update(None)
        return {
            'pose_detected': False,
            'pose_landmarks': [],
            'neck_action': _neck_action_to_dict(neck_action),
        }, landmarker_result

    # 提取 33 个关键点为 [[x, y, z, visibility], ...]
    landmarks_raw = landmarker_result.pose_landmarks[0]
    pose_landmarks = [
        [lm.x, lm.y, lm.z, lm.visibility] for lm in landmarks_raw
    ]

    # 更新颈部动作检测器（list 元素按 (x, y, z, vis) 解包）
    neck_action = neck_detector.update(pose_landmarks)

    return {
        'pose_detected': True,
        'pose_landmarks': pose_landmarks,
        'neck_action': _neck_action_to_dict(neck_action),
    }, landmarker_result


# ============ Visualization ============

def _build_landmark_proto(landmarks_raw):
    """将 PoseLandmarker 的 NormalizedLandmark 列表转换为 protobuf
    NormalizedLandmarkList，以便使用 mp.solutions.drawing_utils 绘制。"""
    landmark_list = landmark_pb2.NormalizedLandmarkList()
    for lm in landmarks_raw:
        landmark = landmark_list.landmark.add()
        landmark.x = lm.x
        landmark.y = lm.y
        landmark.z = lm.z
        if hasattr(lm, 'visibility') and lm.visibility is not None:
            landmark.visibility = lm.visibility
        if hasattr(lm, 'presence') and lm.presence is not None:
            landmark.presence = lm.presence
    return landmark_list


def draw_pose_visualization(image, result, pose_landmarker_result=None):
    """绘制姿态骨骼 + 颈部动作信息。

    Args:
        image: 原始（未翻转）BGR 图像
        result: detect_pose 返回的 result 字典
        pose_landmarker_result: 原始 PoseLandmarkerResult（用于绘制骨骼，
            避免重复检测）。若为 None 且需要绘制骨骼，会回退为不绘制。
    """
    # 水平翻转（镜像效果），与检测时一致
    vis_image = cv2.flip(image.copy(), 1)

    pose_detected = result.get('pose_detected', False)
    neck_action = result.get('neck_action', {})

    if not pose_detected:
        vis_image = put_chinese_text(vis_image, "未检测到姿态",
                                     (10, 10), (128, 128, 128), 24)
        return vis_image

    # 使用 mp.solutions.drawing_utils 绘制姿态骨骼
    if pose_landmarker_result is not None and pose_landmarker_result.pose_landmarks:
        try:
            landmark_list = _build_landmark_proto(
                pose_landmarker_result.pose_landmarks[0])
            mp_drawing.draw_landmarks(
                vis_image,
                landmark_list,
                mp_pose.POSE_CONNECTIONS,
                landmark_drawing_spec=mp_drawing.DrawingSpec(
                    color=(0, 255, 255), thickness=2, circle_radius=3),
                connection_drawing_spec=mp_drawing.DrawingSpec(
                    color=(255, 128, 0), thickness=2),
            )
        except Exception as e:
            print(f"[WARN] Draw landmarks failed: {e}", flush=True)

    # 绘制颈部动作信息（中文）
    action_name = neck_action.get('name', 'None')
    angle = neck_action.get('angle', 0.0)
    amp_score = neck_action.get('amplitude_score', 0.0)
    hold_score = neck_action.get('hold_score', 0.0)
    total_score = neck_action.get('total_score', 0.0)
    hold_duration = neck_action.get('hold_duration', 0.0)

    action_cn = NECK_ACTION_NAMES_CN.get(action_name, action_name)
    text_color = NECK_ACTION_COLORS.get(action_name, (200, 200, 200))

    y = 10
    line_h = 30

    vis_image = put_chinese_text(vis_image, f"动作: {action_cn}",
                                 (10, y), text_color, 26)
    y += line_h + 4
    vis_image = put_chinese_text(vis_image, f"角度: {angle:.1f}°",
                                 (10, y), (255, 255, 255), 24)
    y += line_h
    vis_image = put_chinese_text(vis_image, f"幅度评分: {amp_score:.1f}",
                                 (10, y), (0, 255, 255), 24)
    y += line_h
    vis_image = put_chinese_text(vis_image, f"保持评分: {hold_score:.1f}",
                                 (10, y), (255, 200, 0), 24)
    y += line_h
    vis_image = put_chinese_text(vis_image, f"综合评分: {total_score:.1f}",
                                 (10, y), (0, 255, 0), 26)
    y += line_h
    vis_image = put_chinese_text(vis_image, f"保持时间: {hold_duration:.1f}s",
                                 (10, y), (200, 200, 200), 24)

    return vis_image


# ============ API 接口 ============

@app.route('/health', methods=['GET'])
def health():
    return jsonify({'status': 'ok'})


@app.route('/detect', methods=['POST'])
def detect():
    start_time_req = time.time()

    data = request.json
    image_base64 = data.get('image')

    if not image_base64:
        return jsonify({'success': False, 'error': 'No image provided'}), 400

    try:
        image = decode_image(image_base64)
        if image is None:
            return jsonify({'success': False, 'error': 'Failed to decode image'}), 400

        result, _ = detect_pose(image)
        processing_time = time.time() - start_time_req

        return jsonify({
            'success': True,
            'result': result,
            'processing_time': processing_time
        })
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    start_time_req = time.time()

    data = request.json
    image_base64 = data.get('image')

    if not image_base64:
        return jsonify({'success': False, 'error': 'No image provided'}), 400

    try:
        image = decode_image(image_base64)
        if image is None:
            return jsonify({'success': False, 'error': 'Failed to decode image'}), 400

        result, landmarker_result = detect_pose(image)
        vis_image = draw_pose_visualization(image, result, landmarker_result)

        _, buffer = cv2.imencode('.jpg', vis_image, [cv2.IMWRITE_JPEG_QUALITY, 80])
        vis_base64 = base64.b64encode(buffer).decode('utf-8')

        processing_time = time.time() - start_time_req

        return jsonify({
            'success': True,
            'result': result,
            'visualized_image': vis_base64,
            'processing_time': processing_time
        })
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/reset', methods=['POST'])
def reset():
    neck_detector.reset()
    return jsonify({'success': True, 'message': 'Neck exercise detector reset'})


if __name__ == '__main__':
    print(f"[INFO] Pose detector (PoseLandmarker IMAGE mode) starting on port {PORT}", flush=True)
    app.run(host='0.0.0.0', port=PORT, threaded=True)
