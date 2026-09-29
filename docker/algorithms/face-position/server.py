import os
import time
import base64
import threading
import math
import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
from mediapipe.framework.formats import landmark_pb2
from flask import Flask, request, jsonify
from PIL import Image, ImageDraw, ImageFont

app = Flask(__name__)

PORT = int(os.environ.get('PORT', 9012))

# MediaPipe 绘图工具
mp_drawing = mp.solutions.drawing_utils
mp_face_mesh = mp.solutions.face_mesh

# ============ MediaPipe Face Mesh / Pose 关键点索引 ============
NOSE_TIP = 1                  # 鼻尖
LEFT_EYE_OUTER = 33           # 左眼外角
RIGHT_EYE_OUTER = 263         # 右眼外角

POSE_NOSE = 0
POSE_LEFT_SHOULDER = 11
POSE_RIGHT_SHOULDER = 12


# ============ 面部空间坐标检测结果 ============
class FacePosition:
    """面部空间坐标检测结果"""
    def __init__(self, x_cm=0.0, y_cm=0.0, z_cm=0.0, distance_cm=0.0, confidence=0.0, valid=False):
        self.x_cm = x_cm              # 水平偏移：+ 向右 / - 向左
        self.y_cm = y_cm              # 垂直偏移：+ 向上 / - 向下
        self.z_cm = z_cm              # 鼻尖相对双肩深度差：+ 低头 / - 后仰
        self.distance_cm = distance_cm # 到摄像头绝对距离估算（cm）
        self.confidence = confidence  # 置信度 0~1
        self.valid = valid            # 是否有效输出

    @classmethod
    def invalid(cls):
        return cls()

    def to_dict(self):
        return {
            'x_cm': round(self.x_cm, 1),
            'y_cm': round(self.y_cm, 1),
            'z_cm': round(self.z_cm, 1),
            'distance_cm': round(self.distance_cm, 1),
            'confidence': round(self.confidence, 3),
            'valid': self.valid,
        }


class FacePositionEstimator:
    """Face Mesh + Pose 融合的面部空间坐标估计器"""

    # Configuration
    PUPIL_DISTANCE_CM = 6.3        # 瞳距物理先验
    HEAD_LENGTH_CM = 23.0          # 头长物理先验
    CAMERA_FOV_DEGREES = 60.0      # 摄像头FOV
    MIN_VISIBILITY = 0.5           # 关键点最小可见度
    SMOOTHING_ALPHA = 0.3          # 一阶低通滤波系数
    OUTLIER_JUMP_THRESHOLD_CM = 30.0  # 异常跳变阈值
    INVALID_RESET_FRAMES = 10      # 连续无效帧重置阈值

    def __init__(self):
        # 预计算针孔模型系数
        fov_rad = math.radians(self.CAMERA_FOV_DEGREES)
        self._pinhole_k = 1.0 / (2.0 * math.tan(fov_rad / 2.0))
        # 滤波状态
        self._last_valid_pos = None
        self._invalid_count = 0

    def update(self, face_landmarks, pose_landmarks, image_w, image_h):
        """
        Args:
            face_landmarks: List of NormalizedLandmark from FaceLandmarker (478 points)
            pose_landmarks: List of (x, y, z, visibility) tuples from PoseLandmarker (33 points)
            image_w: image width
            image_h: image height
        Returns:
            FacePosition
        """
        # 1. Check Face Mesh validity
        if not face_landmarks or len(face_landmarks) == 0:
            return self._handle_invalid()

        # 2. Check Pose validity
        if not pose_landmarks or len(pose_landmarks) == 0:
            return self._handle_invalid()

        # 3. Index check
        needed_face_indices = [NOSE_TIP, LEFT_EYE_OUTER, RIGHT_EYE_OUTER]
        if any(idx >= len(face_landmarks) for idx in needed_face_indices):
            return self._handle_invalid()
        if POSE_NOSE >= len(pose_landmarks) or POSE_LEFT_SHOULDER >= len(pose_landmarks) or POSE_RIGHT_SHOULDER >= len(pose_landmarks):
            return self._handle_invalid()

        # 4. Pose visibility check
        pose_nose = pose_landmarks[POSE_NOSE]
        pose_l_sh = pose_landmarks[POSE_LEFT_SHOULDER]
        pose_r_sh = pose_landmarks[POSE_RIGHT_SHOULDER]

        nose_vis = pose_nose[3] if len(pose_nose) >= 4 else 0
        l_sh_vis = pose_l_sh[3] if len(pose_l_sh) >= 4 else 0
        r_sh_vis = pose_r_sh[3] if len(pose_r_sh) >= 4 else 0

        if min(nose_vis, l_sh_vis, r_sh_vis) < self.MIN_VISIBILITY:
            return self._handle_invalid()

        # 5. Calculate pupil distance (normalized)
        left_eye = face_landmarks[LEFT_EYE_OUTER]
        right_eye = face_landmarks[RIGHT_EYE_OUTER]
        nose_face = face_landmarks[NOSE_TIP]

        # face_landmarks items can be NormalizedLandmark (with .x, .y) or tuples
        def get_xy(lm):
            if hasattr(lm, 'x'):
                return lm.x, lm.y
            return lm[0], lm[1]

        left_x, left_y = get_xy(left_eye)
        right_x, right_y = get_xy(right_eye)
        nose_x, nose_y = get_xy(nose_face)

        pupil_dist_norm = math.sqrt((right_x - left_x) ** 2 + (right_y - left_y) ** 2)
        if pupil_dist_norm < 0.005:
            return self._handle_invalid()

        # 6. Calculate X/Y (cm) - using normalized coords, image-size independent
        # Origin: image geometric center (0.5, 0.5)
        cx_norm, cy_norm = 0.5, 0.5
        x_cm = (nose_x - cx_norm) / pupil_dist_norm * self.PUPIL_DISTANCE_CM
        y_cm = -(nose_y - cy_norm) / pupil_dist_norm * self.PUPIL_DISTANCE_CM  # Y negative = up

        # 7. Calculate Z (cm) - Pose z difference * head length prior
        nose_z = pose_nose[2]
        mid_shoulder_z = (pose_l_sh[2] + pose_r_sh[2]) / 2
        z_diff = nose_z - mid_shoulder_z
        z_cm = -z_diff * self.HEAD_LENGTH_CM  # + = looking down

        # 8. Calculate distance_cm - pinhole model + FOV assumption
        distance_cm = self._pinhole_k * self.PUPIL_DISTANCE_CM / pupil_dist_norm

        # 9. Confidence
        confidence = (nose_vis + l_sh_vis + r_sh_vis) / 3

        # 10. Outlier rejection
        new_pos = FacePosition(x_cm=x_cm, y_cm=y_cm, z_cm=z_cm, distance_cm=distance_cm, confidence=confidence, valid=True)
        if self._is_outlier(new_pos):
            return self._last_valid_pos if self._last_valid_pos else FacePosition.invalid()

        # 11. Low-pass filter
        if self._last_valid_pos is not None and self._last_valid_pos.valid:
            alpha = self.SMOOTHING_ALPHA
            new_pos.x_cm = alpha * new_pos.x_cm + (1 - alpha) * self._last_valid_pos.x_cm
            new_pos.y_cm = alpha * new_pos.y_cm + (1 - alpha) * self._last_valid_pos.y_cm
            new_pos.z_cm = alpha * new_pos.z_cm + (1 - alpha) * self._last_valid_pos.z_cm
            new_pos.distance_cm = alpha * new_pos.distance_cm + (1 - alpha) * self._last_valid_pos.distance_cm

        self._last_valid_pos = new_pos
        self._invalid_count = 0
        return new_pos

    def _is_outlier(self, new_pos):
        if self._last_valid_pos is None or not self._last_valid_pos.valid:
            return False
        threshold = self.OUTLIER_JUMP_THRESHOLD_CM
        dx = abs(new_pos.x_cm - self._last_valid_pos.x_cm)
        dy = abs(new_pos.y_cm - self._last_valid_pos.y_cm)
        dz = abs(new_pos.z_cm - self._last_valid_pos.z_cm)
        d_dist = abs(new_pos.distance_cm - self._last_valid_pos.distance_cm)
        return (dx > threshold or dy > threshold or dz > threshold or d_dist > threshold * 2)

    def _handle_invalid(self):
        self._invalid_count += 1
        if self._invalid_count >= self.INVALID_RESET_FRAMES:
            self._last_valid_pos = None
            self._invalid_count = 0
            return FacePosition.invalid()
        if self._last_valid_pos is not None:
            return FacePosition(
                x_cm=self._last_valid_pos.x_cm,
                y_cm=self._last_valid_pos.y_cm,
                z_cm=self._last_valid_pos.z_cm,
                distance_cm=self._last_valid_pos.distance_cm,
                confidence=0.0,
                valid=False,
            )
        return FacePosition.invalid()

    def reset(self):
        self._last_valid_pos = None
        self._invalid_count = 0


# 全局估计器实例（有状态：低通滤波 + 异常剔除）
estimator = FacePositionEstimator()
estimator_lock = threading.Lock()


# ============ FaceLandmarker 初始化 ============
MODEL_PATH = os.environ.get('MODEL_PATH', 'face_landmarker.task')

base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
_face_options = vision.FaceLandmarkerOptions(
    base_options=base_options,
    running_mode=vision.RunningMode.IMAGE,
    num_faces=1,
    min_face_detection_confidence=0.5,
    min_face_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
face_landmarker = vision.FaceLandmarker.create_from_options(_face_options)
face_lock = threading.Lock()

print(f"[INFO] FaceLandmarker loaded (IMAGE mode), model={MODEL_PATH}", flush=True)


# ============ PIL 中文绘制工具 ============
_font_cache = {}


def _get_font(size=24):
    """获取支持中文的字体"""
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


# ============ 图像解码 ============
def decode_image(image_base64):
    """解码 base64 图像"""
    img_bytes = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_bytes, np.uint8)
    return cv2.imdecode(nparr, cv2.IMREAD_COLOR)


# ============ 面部位置检测 ============
def detect_face_position(image, pose_landmarks=None):
    """运行 FaceLandmarker + FacePositionEstimator

    Returns:
        (result_dict, face_landmarks_list, flipped_image)
    """
    h, w = image.shape[:2]

    # 1. 水平翻转（镜像）
    flipped = cv2.flip(image, 1)

    # 2. 转 RGB 给 MediaPipe
    rgb = cv2.cvtColor(flipped, cv2.COLOR_BGR2RGB)
    mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

    # 3. 运行 FaceLandmarker（IMAGE 同步模式）
    with face_lock:
        face_result = face_landmarker.detect(mp_image)

    face_detected = bool(face_result.face_landmarks) and len(face_result.face_landmarks) > 0

    face_lms = None
    if face_detected:
        face_lms = face_result.face_landmarks[0]  # 478 个关键点

    # 4. 估计面部位置（需 face + pose 同时有效）
    pos = FacePosition.invalid()
    if face_detected and pose_landmarks:
        with estimator_lock:
            pos = estimator.update(face_lms, pose_landmarks, w, h)
    # 无 pose_landmarks 时仅运行 face_landmarker，不估计位置

    result = {
        'valid': pos.valid,
        'face_detected': face_detected,
        'x_cm': round(pos.x_cm, 1),
        'y_cm': round(pos.y_cm, 1),
        'z_cm': round(pos.z_cm, 1),
        'distance_cm': round(pos.distance_cm, 1),
        'confidence': round(pos.confidence, 3),
    }

    return result, face_lms, flipped


# ============ 可视化 ============
def draw_face_mesh(image, face_landmarks):
    """绘制 face mesh（tessellation + contours）"""
    if not face_landmarks:
        return image
    try:
        proto = landmark_pb2.NormalizedLandmarkList()
        for lm in face_landmarks:
            point = proto.landmark.add()
            point.x = lm.x
            point.y = lm.y
            point.z = lm.z if hasattr(lm, 'z') else 0.0

        # Tessellation
        mp_drawing.draw_landmarks(
            image=image,
            landmark_list=proto,
            connections=mp_face_mesh.FACEMESH_TESSELLATION,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(color=(48, 255, 48), thickness=1),
        )
        # Contours
        mp_drawing.draw_landmarks(
            image=image,
            landmark_list=proto,
            connections=mp_face_mesh.FACEMESH_CONTOURS,
            landmark_drawing_spec=mp_drawing.DrawingSpec(color=(0, 0, 255), thickness=1, circle_radius=1),
            connection_drawing_spec=mp_drawing.DrawingSpec(color=(0, 255, 255), thickness=2),
        )
    except Exception as e:
        print(f"[WARN] Face mesh drawing failed: {e}", flush=True)
    return image


def draw_position_visualization(image, result, face_landmarks):
    """绘制 face mesh + 位置信息"""
    vis_image = image.copy()

    # 绘制 face mesh
    if face_landmarks:
        draw_face_mesh(vis_image, face_landmarks)

    valid = result.get('valid', False)

    y_offset = 10
    if not valid:
        vis_image = put_chinese_text(vis_image, "无法检测", (10, y_offset), (0, 0, 255), 28)
        return vis_image

    x_cm = result.get('x_cm', 0)
    y_cm = result.get('y_cm', 0)
    z_cm = result.get('z_cm', 0)
    distance_cm = result.get('distance_cm', 0)
    confidence = result.get('confidence', 0)

    vis_image = put_chinese_text(vis_image, "面部位置", (10, y_offset), (0, 255, 255), 28)
    y_offset += 34
    vis_image = put_chinese_text(vis_image, f"X: {x_cm} cm (水平偏移)", (10, y_offset), (255, 255, 255), 22)
    y_offset += 28
    vis_image = put_chinese_text(vis_image, f"Y: {y_cm} cm (垂直偏移)", (10, y_offset), (255, 255, 255), 22)
    y_offset += 28
    vis_image = put_chinese_text(vis_image, f"Z: {z_cm} cm (深度差)", (10, y_offset), (255, 255, 255), 22)
    y_offset += 28
    vis_image = put_chinese_text(vis_image, f"距离: {distance_cm} cm", (10, y_offset), (0, 255, 0), 22)
    y_offset += 28
    vis_image = put_chinese_text(vis_image, f"置信度: {confidence}", (10, y_offset), (0, 200, 255), 22)

    return vis_image


# ============ API 接口 ============
@app.route('/health', methods=['GET'])
def health():
    return jsonify({'status': 'ok'})


@app.route('/detect', methods=['POST'])
def detect():
    start_time = time.time()

    data = request.json or {}
    image_base64 = data.get('image')
    pose_landmarks = data.get('pose_landmarks')

    if not image_base64:
        return jsonify({'success': False, 'error': 'No image provided'}), 400

    try:
        image = decode_image(image_base64)
        if image is None:
            return jsonify({'success': False, 'error': 'Failed to decode image'}), 400

        result, _, _ = detect_face_position(image, pose_landmarks)
        processing_time = time.time() - start_time

        return jsonify({
            'success': True,
            'result': result,
            'processing_time': processing_time
        })
    except Exception as e:
        print(f"[ERROR] /detect failed: {e}", flush=True)
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    start_time = time.time()

    data = request.json or {}
    image_base64 = data.get('image')
    pose_landmarks = data.get('pose_landmarks')

    if not image_base64:
        return jsonify({'success': False, 'error': 'No image provided'}), 400

    try:
        image = decode_image(image_base64)
        if image is None:
            return jsonify({'success': False, 'error': 'Failed to decode image'}), 400

        result, face_lms, flipped = detect_face_position(image, pose_landmarks)
        vis_image = draw_position_visualization(flipped, result, face_lms)

        _, buffer = cv2.imencode('.jpg', vis_image, [cv2.IMWRITE_JPEG_QUALITY, 80])
        vis_base64 = base64.b64encode(buffer).decode('utf-8')

        processing_time = time.time() - start_time

        return jsonify({
            'success': True,
            'result': result,
            'visualized_image': vis_base64,
            'processing_time': processing_time
        })
    except Exception as e:
        print(f"[ERROR] /detect_visualize failed: {e}", flush=True)
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/reset', methods=['POST'])
def reset():
    with estimator_lock:
        estimator.reset()
    return jsonify({'success': True, 'message': 'Face position estimator reset'})


if __name__ == '__main__':
    print(f"[INFO] Face position service starting on port {PORT}", flush=True)
    app.run(host='0.0.0.0', port=PORT, threaded=True)
