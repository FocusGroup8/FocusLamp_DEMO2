from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import onnxruntime as ort
from itertools import product as product
from math import ceil

app = Flask(__name__)

ONNX_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'weights', 'slim_Final.onnx')
onnx_session = None

DEADZONE_THRESHOLD = 20.0
VIS_THRESHOLD = 0.75
prev_bbox = None
prev_landmarks = None

cfg_slim = {
    'name': 'slim',
    'min_sizes': [[10, 16, 24], [32, 48], [64, 96], [128, 192, 256]],
    'steps': [8, 16, 32, 64],
    'variance': [0.1, 0.2],
    'clip': False,
    'image_size': 300
}

CONFIDENCE_THRESHOLD = 0.02
TOP_K = 5000
NMS_THRESHOLD = 0.4
KEEP_TOP_K = 750
VIS_THRESHOLD = 0.6
LONG_SIDE = 300


def load_onnx_model():
    global onnx_session
    if onnx_session is None:
        try:
            providers = []
            available = ort.get_available_providers()
            if 'CUDAExecutionProvider' in available:
                providers.append(('CUDAExecutionProvider', {
                    'device_id': 0,
                    'arena_extend_strategy': 'kNextPowerOfTwo',
                    'gpu_mem_limit': 2 * 1024 * 1024 * 1024,
                    'cudnn_conv_algo_search': 'EXHAUSTIVE',
                }))
            providers.append('CPUExecutionProvider')
            onnx_session = ort.InferenceSession(ONNX_MODEL_PATH, providers=providers)
            active_provider = onnx_session.get_providers()
            print(f"[INFO] Face-Detector ONNX model loaded, providers: {active_provider}")
        except Exception as e:
            print(f"[ERROR] Failed to load ONNX model: {e}")


class PriorBox:
    def __init__(self, cfg, image_size=None):
        self.min_sizes = cfg['min_sizes']
        self.steps = cfg['steps']
        self.clip = cfg['clip']
        self.image_size = image_size
        self.feature_maps = [[ceil(self.image_size[0] / step), ceil(self.image_size[1] / step)] for step in self.steps]

    def forward(self):
        anchors = []
        for k, f in enumerate(self.feature_maps):
            min_sizes = self.min_sizes[k]
            for i, j in product(range(f[0]), range(f[1])):
                for min_size in min_sizes:
                    s_kx = min_size / self.image_size[1]
                    s_ky = min_size / self.image_size[0]
                    dense_cx = [x * self.steps[k] / self.image_size[1] for x in [j + 0.5]]
                    dense_cy = [y * self.steps[k] / self.image_size[0] for y in [i + 0.5]]
                    for cy, cx in product(dense_cy, dense_cx):
                        anchors += [cx, cy, s_kx, s_ky]
        output = np.array(anchors, dtype=np.float32).reshape(-1, 4)
        if self.clip:
            output = np.clip(output, 0, 1)
        return output


def decode(loc, priors, variances):
    boxes = np.concatenate([
        priors[:, :2] + loc[:, :2] * variances[0] * priors[:, 2:],
        priors[:, 2:] * np.exp(loc[:, 2:] * variances[1])
    ], axis=1)
    boxes[:, :2] -= boxes[:, 2:] / 2
    boxes[:, 2:] += boxes[:, :2]
    return boxes


def decode_landm(pre, priors, variances):
    landms = np.concatenate([
        priors[:, :2] + pre[:, :2] * variances[0] * priors[:, 2:],
        priors[:, :2] + pre[:, 2:4] * variances[0] * priors[:, 2:],
        priors[:, :2] + pre[:, 4:6] * variances[0] * priors[:, 2:],
        priors[:, :2] + pre[:, 6:8] * variances[0] * priors[:, 2:],
        priors[:, :2] + pre[:, 8:10] * variances[0] * priors[:, 2:],
    ], axis=1)
    return landms


def py_cpu_nms(dets, thresh):
    x1 = dets[:, 0]
    y1 = dets[:, 1]
    x2 = dets[:, 2]
    y2 = dets[:, 3]
    scores = dets[:, 4]
    areas = (x2 - x1 + 1) * (y2 - y1 + 1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0.0, xx2 - xx1 + 1)
        h = np.maximum(0.0, yy2 - yy1 + 1)
        inter = w * h
        ovr = inter / (areas[i] + areas[order[1:]] - inter)
        inds = np.where(ovr <= thresh)[0]
        order = order[inds + 1]
    return keep


def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image


def detect_faces(image):
    global onnx_session

    if onnx_session is None:
        return {"face_count": 0, "faces": []}

    image = cv2.flip(image, 1)

    orig_height, orig_width = image.shape[:2]
    
    target_size = 300
    img = cv2.resize(image, (target_size, target_size), interpolation=cv2.INTER_LINEAR)
    im_height, im_width, _ = img.shape

    scale = np.array([orig_width, orig_height, orig_width, orig_height], dtype=np.float32)
    img = np.float32(img)
    img -= (104, 117, 123)
    img = img.transpose(2, 0, 1)
    img = np.expand_dims(img, 0).astype(np.float32)

    input_name = onnx_session.get_inputs()[0].name
    loc, conf, landms = onnx_session.run(None, {input_name: img})

    loc = loc[0]
    conf = conf[0]
    landms = landms[0]

    priorbox = PriorBox(cfg_slim, image_size=(im_height, im_width))
    priors = priorbox.forward()

    boxes = decode(loc, priors, cfg_slim['variance'])
    boxes = boxes * scale
    scores = conf[:, 1]
    landms = decode_landm(landms, priors, cfg_slim['variance'])
    scale1 = np.array([orig_width, orig_height] * 5, dtype=np.float32)
    landms = landms * scale1

    inds = np.where(scores > CONFIDENCE_THRESHOLD)[0]
    boxes = boxes[inds]
    landms = landms[inds]
    scores = scores[inds]

    order = scores.argsort()[::-1][:TOP_K]
    boxes = boxes[order]
    landms = landms[order]
    scores = scores[order]

    dets = np.hstack((boxes, scores[:, np.newaxis])).astype(np.float32, copy=False)
    keep = py_cpu_nms(dets, NMS_THRESHOLD)
    dets = dets[keep, :]
    landms = landms[keep]
    dets = dets[:KEEP_TOP_K, :]
    landms = landms[:KEEP_TOP_K, :]

    faces = []
    global prev_bbox, prev_landmarks
    
    if dets.shape[0] == 0:
        prev_bbox = None
        prev_landmarks = None
        return {"face_count": 0, "faces": []}
    
    best_idx = 0
    best_conf = dets[0][4]
    for i in range(1, dets.shape[0]):
        if dets[i][4] > best_conf:
            best_conf = dets[i][4]
            best_idx = i
    
    if dets[best_idx][4] < VIS_THRESHOLD:
        prev_bbox = None
        prev_landmarks = None
        return {"face_count": 0, "faces": []}
    
    current_bbox = dets[best_idx][:4].astype(np.float32)
    current_landmarks = landms[best_idx].astype(np.float32)
    
    if prev_bbox is not None:
        diff_bbox = np.abs(current_bbox - prev_bbox)
        mask_bbox = diff_bbox < DEADZONE_THRESHOLD
        current_bbox = np.where(mask_bbox, prev_bbox, current_bbox)
    if prev_landmarks is not None:
        diff_landmarks = np.abs(current_landmarks - prev_landmarks)
        mask_landmarks = diff_landmarks < DEADZONE_THRESHOLD
        current_landmarks = np.where(mask_landmarks, prev_landmarks, current_landmarks)
    
    prev_bbox = current_bbox.copy()
    prev_landmarks = current_landmarks.copy()
    
    bbox = [int(current_bbox[0]), int(current_bbox[1]), int(current_bbox[2]), int(current_bbox[3])]
    confidence = float(dets[best_idx][4])
    
    center_x = orig_width / 2
    center_y = orig_height / 2
    
    landmarks_pixel = {
        "left_eye": [int(current_landmarks[0]), int(current_landmarks[1])],
        "right_eye": [int(current_landmarks[2]), int(current_landmarks[3])],
        "nose": [int(current_landmarks[4]), int(current_landmarks[5])],
        "left_mouth": [int(current_landmarks[6]), int(current_landmarks[7])],
        "right_mouth": [int(current_landmarks[8]), int(current_landmarks[9])]
    }
    
    landmarks = {
        "left_eye": [int(current_landmarks[0] - center_x), int(center_y - current_landmarks[1])],
        "right_eye": [int(current_landmarks[2] - center_x), int(center_y - current_landmarks[3])],
        "nose": [int(current_landmarks[4] - center_x), int(center_y - current_landmarks[5])],
        "left_mouth": [int(current_landmarks[6] - center_x), int(center_y - current_landmarks[7])],
        "right_mouth": [int(current_landmarks[8] - center_x), int(center_y - current_landmarks[9])]
    }
    
    faces.append({
        "bbox": bbox,
        "confidence": round(confidence, 4),
        "landmarks": landmarks,
        "landmarks_pixel": landmarks_pixel,
        "image_size": {"width": orig_width, "height": orig_height}
    })

    return {
        "face_count": len(faces),
        "faces": faces
    }


def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
    (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
    cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2), 
                 (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
    cv2.putText(img, text, pos, font, scale, color, thickness)

def draw_face_visualization(image, result):
    vis_image = cv2.flip(image.copy(), 1)

    face_count = result.get('face_count', 0)
    draw_text_with_bg(vis_image, f"Faces: {face_count}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

    for face in result.get('faces', []):
        bbox = face['bbox']
        confidence = face['confidence']
        landmarks = face.get('landmarks_pixel', face['landmarks'])

        cv2.rectangle(vis_image, (bbox[0], bbox[1]), (bbox[2], bbox[3]), (0, 0, 255), 2)
        text = f"{confidence:.4f}"
        draw_text_with_bg(vis_image, text, (bbox[0], bbox[1] - 10),
                    cv2.FONT_HERSHEY_DUPLEX, 0.5, (255, 255, 255), 1)

        cv2.circle(vis_image, tuple(landmarks['left_eye']), 1, (0, 0, 255), 4)
        cv2.circle(vis_image, tuple(landmarks['right_eye']), 1, (0, 255, 255), 4)
        cv2.circle(vis_image, tuple(landmarks['nose']), 1, (255, 0, 255), 4)
        cv2.circle(vis_image, tuple(landmarks['left_mouth']), 1, (0, 255, 0), 4)
        cv2.circle(vis_image, tuple(landmarks['right_mouth']), 1, (255, 0, 0), 4)

    return vis_image


@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "face_detector",
        "version": "3.0.0",
        "method": "Face-Detector-1MB_Slim_ONNX"
    })


@app.route('/detect', methods=['POST'])
def detect():
    start_time = time.time()

    data = request.json
    image_base64 = data.get('image')

    if not image_base64:
        return jsonify({
            "success": False,
            "error": "No image provided"
        }), 400

    try:
        image = decode_image(image_base64)
        if image is None:
            return jsonify({
                "success": False,
                "error": "Failed to decode image"
            }), 500
        result = detect_faces(image)

        processing_time = time.time() - start_time

        return jsonify({
            "success": True,
            "result": result,
            "processing_time": processing_time
        })
    except Exception as e:
        import traceback
        traceback.print_exc()
        return jsonify({
            "success": False,
            "error": str(e)
        }), 500


@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    start_time = time.time()

    data = request.json
    image_base64 = data.get('image')

    if not image_base64:
        return jsonify({
            "success": False,
            "error": "No image provided"
        }), 400

    try:
        image = decode_image(image_base64)
        result = detect_faces(image)

        vis_image = draw_face_visualization(image, result)

        _, buffer = cv2.imencode('.jpg', vis_image, [cv2.IMWRITE_JPEG_QUALITY, 80])
        vis_base64 = base64.b64encode(buffer).decode('utf-8')

        processing_time = time.time() - start_time

        return jsonify({
            "success": True,
            "result": result,
            "visualized_image": vis_base64,
            "processing_time": processing_time
        })
    except Exception as e:
        return jsonify({
            "success": False,
            "error": str(e)
        }), 500


load_onnx_model()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 8006))
    app.run(host='0.0.0.0', port=port)
