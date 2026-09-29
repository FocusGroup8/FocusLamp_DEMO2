import os
import time
import base64
import threading
import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
from mediapipe.framework.formats import landmark_pb2
from flask import Flask, request, jsonify
from PIL import Image, ImageDraw, ImageFont

app = Flask(__name__)

PORT = int(os.environ.get('PORT', 9011))

# 模型路径（与 server.py 同目录）
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'face_landmarker.task')

# MediaPipe FaceLandmarker 初始化（IMAGE 同步模式）
base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
options = vision.FaceLandmarkerOptions(
    base_options=base_options,
    running_mode=vision.RunningMode.IMAGE,
    num_faces=1,
    min_face_detection_confidence=0.5,
    min_face_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
landmarker = vision.FaceLandmarker.create_from_options(options)

# 用于绘制人脸网格的连接关系常量
mp_face_mesh = mp.solutions.face_mesh
mp_drawing = mp.solutions.drawing_utils

landmarker_lock = threading.Lock()


# ============ PIL 中文文字绘制工具 ============

_font_cache = {}


def _get_font(size=24):
    """获取支持中文的字体"""
    if size in _font_cache:
        return _font_cache[size]
    font_paths = [
        '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
        '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
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


def decode_image(image_base64):
    """解码 base64 图像"""
    img_bytes = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_bytes, np.uint8)
    return cv2.imdecode(nparr, cv2.IMREAD_COLOR)


def detect_face_landmarks(image):
    """检测人脸关键点

    Args:
        image: BGR 格式的 numpy 图像（已做镜像翻转）

    Returns:
        tuple: (result_dict, face_landmarks_raw)
            result_dict: JSON 可序列化的检测结果
            face_landmarks_raw: 原始 NormalizedLandmark 列表（用于可视化），无人脸时为 None
    """
    global landmarker

    try:
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        with landmarker_lock:
            result = landmarker.detect(mp_image)

        if not result.face_landmarks:
            return {
                'face_detected': False,
                'landmarks_count': 0,
                'face_landmarks': [],
            }, None

        # 取第一个人脸的 478 个关键点（468 人脸 + 10 虹膜）
        face_landmarks = result.face_landmarks[0]
        landmarks_list = [
            [round(lm.x, 6), round(lm.y, 6), round(lm.z, 6)]
            for lm in face_landmarks
        ]

        return {
            'face_detected': True,
            'landmarks_count': len(landmarks_list),
            'face_landmarks': landmarks_list,
        }, face_landmarks

    except Exception as e:
        print(f"[WARN] Face landmark detection failed: {e}", flush=True)
        return {
            'face_detected': False,
            'landmarks_count': 0,
            'face_landmarks': [],
        }, None


def draw_face_visualization(image, detection_result, face_landmarks_raw=None):
    """绘制人脸关键点可视化

    Args:
        image: 原始 BGR 图像（未翻转）
        detection_result: detect_face_landmarks 返回的 result_dict
        face_landmarks_raw: 原始 NormalizedLandmark 列表（可选，避免重复检测）

    Returns:
        可视化后的 BGR 图像
    """
    # 镜像翻转，与检测时的坐标系保持一致
    vis_image = cv2.flip(image.copy(), 1)
    h, w = vis_image.shape[:2]

    face_detected = detection_result.get('face_detected', False)
    landmarks_count = detection_result.get('landmarks_count', 0)

    if not face_detected:
        vis_image = put_chinese_text(vis_image, "未检测到人脸",
                                     (10, 10), (0, 0, 255), 28)
        return vis_image

    # 若未传入原始关键点，则重新检测一次以获取 NormalizedLandmark 对象
    raw_landmarks = face_landmarks_raw
    if raw_landmarks is None:
        try:
            rgb = cv2.cvtColor(vis_image, cv2.COLOR_BGR2RGB)
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
            with landmarker_lock:
                re_result = landmarker.detect(mp_image)
            if re_result.face_landmarks:
                raw_landmarks = re_result.face_landmarks[0]
        except Exception as e:
            print(f"[WARN] Re-detection for visualization failed: {e}", flush=True)
            raw_landmarks = None

    if raw_landmarks is not None:
        # 构造 NormalizedLandmarkList 用于 draw_landmarks
        face_landmarks_proto = landmark_pb2.NormalizedLandmarkList()
        face_landmarks_proto.landmark.extend([
            landmark_pb2.NormalizedLandmark(x=lm.x, y=lm.y, z=lm.z)
            for lm in raw_landmarks
        ])

        # 绘制网格细分
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_TESSELATION,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(48, 48, 48), thickness=1),
        )
        # 绘制脸部轮廓
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_FACE_OVAL,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(0, 255, 0), thickness=2),
        )
        # 左眼
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_LEFT_EYE,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(255, 0, 0), thickness=2),
        )
        # 右眼
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_RIGHT_EYE,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(0, 0, 255), thickness=2),
        )
        # 嘴唇
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_LIPS,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(0, 128, 255), thickness=2),
        )
        # 左虹膜
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_LEFT_IRIS,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(255, 0, 255), thickness=2),
        )
        # 右虹膜
        mp_drawing.draw_landmarks(
            image=vis_image,
            landmark_list=face_landmarks_proto,
            connections=mp_face_mesh.FACEMESH_RIGHT_IRIS,
            landmark_drawing_spec=None,
            connection_drawing_spec=mp_drawing.DrawingSpec(
                color=(255, 255, 0), thickness=2),
        )
        # 绘制所有关键点
        for lm in raw_landmarks:
            cx, cy = int(lm.x * w), int(lm.y * h)
            cv2.circle(vis_image, (cx, cy), 1, (255, 255, 255), -1)

    # 绘制信息文字
    detected_text = "是" if face_detected else "否"
    vis_image = put_chinese_text(vis_image, f"检测到人脸: {detected_text}",
                                 (10, 10), (0, 255, 0), 26)
    vis_image = put_chinese_text(vis_image, f"关键点数: {landmarks_count}",
                                 (10, 42), (255, 255, 0), 24)

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

        # 镜像翻转后再检测
        flipped = cv2.flip(image, 1)
        result, _ = detect_face_landmarks(flipped)
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

        # 检测（内部会做镜像翻转），同时获取原始关键点用于可视化，避免重复检测
        flipped = cv2.flip(image, 1)
        result, face_landmarks_raw = detect_face_landmarks(flipped)
        vis_image = draw_face_visualization(image, result, face_landmarks_raw)

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
    # FaceLandmarker 为无状态同步模式，无状态需重置，保留接口以保持一致性
    return jsonify({'success': True, 'message': 'Face landmarker reset'})


if __name__ == '__main__':
    print(f"[INFO] Face landmarker starting on port {PORT}", flush=True)
    app.run(host='0.0.0.0', port=PORT, threaded=True)
