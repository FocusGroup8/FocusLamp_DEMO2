from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import onnxruntime as ort
from collections import deque

app = Flask(__name__)

ONNX_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'resnet_50_size-256.onnx')
HAND_LANDMARKER_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hand_landmarker.task')
onnx_session = None
hand_landmarker = None
IMG_SIZE = 256

SMOOTHING_ALPHA = 0.3
KEYPOINT_HISTORY_SIZE = 5
prev_keypoints = None
keypoint_history = deque(maxlen=KEYPOINT_HISTORY_SIZE)

MEDIAPIPE_AVAILABLE = False

try:
    import mediapipe as mp
    from mediapipe.tasks import python
    from mediapipe.tasks.python import vision
    MEDIAPIPE_AVAILABLE = True
    print("[INFO] MediaPipe new API available")
except Exception as e:
    print(f"[WARN] MediaPipe not available: {e}")

FINGER_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4),
    (0, 5), (5, 6), (6, 7), (7, 8),
    (0, 9), (9, 10), (10, 11), (11, 12),
    (0, 13), (13, 14), (14, 15), (15, 16),
    (0, 17), (17, 18), (18, 19), (19, 20),
    (5, 9), (9, 13), (13, 17)
]

def load_onnx_model():
    global onnx_session, hand_landmarker, MEDIAPIPE_AVAILABLE
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
            print(f"[INFO] ONNX handpose model loaded, providers: {active_provider}")
        except Exception as e:
            print(f"[ERROR] Failed to load ONNX model: {e}")
    
    if MEDIAPIPE_AVAILABLE and hand_landmarker is None:
        try:
            import mediapipe as mp
            from mediapipe.tasks import python
            from mediapipe.tasks.python import vision
            
            BaseOptions = mp.tasks.BaseOptions
            HandLandmarker = mp.tasks.vision.HandLandmarker
            HandLandmarkerOptions = mp.tasks.vision.HandLandmarkerOptions
            VisionRunningMode = mp.tasks.vision.RunningMode
            
            if os.path.exists(HAND_LANDMARKER_MODEL_PATH):
                options = HandLandmarkerOptions(
                    base_options=BaseOptions(model_asset_path=HAND_LANDMARKER_MODEL_PATH),
                    running_mode=VisionRunningMode.IMAGE,
                    num_hands=1,
                    min_hand_detection_confidence=0.5,
                    min_hand_presence_confidence=0.5,
                    min_tracking_confidence=0.5)
                hand_landmarker = HandLandmarker.create_from_options(options)
                print("[INFO] MediaPipe HandLandmarker loaded")
            else:
                print(f"[WARN] Hand landmarker model not found: {HAND_LANDMARKER_MODEL_PATH}")
                MEDIAPIPE_AVAILABLE = False
        except Exception as e:
            print(f"[WARN] Failed to load MediaPipe HandLandmarker: {e}")
            MEDIAPIPE_AVAILABLE = False

def decode_image(image_base64):
    try:
        img_data = base64.b64decode(image_base64)
        nparr = np.frombuffer(img_data, np.uint8)
        image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
        if image is None:
            print(f"[ERROR] Failed to decode image, data length: {len(img_data)}")
        return image
    except Exception as e:
        print(f"[ERROR] decode_image exception: {e}")
        return None

def center_crop_to_square(image):
    h, w = image.shape[:2]
    size = min(h, w)
    start_x = (w - size) // 2
    start_y = (h - size) // 2
    return image[start_y:start_y + size, start_x:start_x + size]

def detect_hand_presence(image):
    """使用 MediaPipe HandLandmarker 检测图像中是否有手"""
    global hand_landmarker, MEDIAPIPE_AVAILABLE
    
    if not MEDIAPIPE_AVAILABLE or hand_landmarker is None:
        return None, 0.0
    
    try:
        import mediapipe as mp
        
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        
        result = hand_landmarker.detect(mp_image)
        
        if result.hand_landmarks and len(result.hand_landmarks) > 0:
            landmarks = result.hand_landmarks[0]
            handedness = result.handedness[0] if result.handedness else None
            
            confidence = 0.0
            if handedness:
                confidence = handedness[0].score
            
            h, w = image.shape[:2]
            x_coords = [lm.x * w for lm in landmarks]
            y_coords = [lm.y * h for lm in landmarks]
            
            bbox = {
                'x_min': max(0, min(x_coords)),
                'y_min': max(0, min(y_coords)),
                'x_max': min(w, max(x_coords)),
                'y_max': min(h, max(y_coords))
            }
            
            return bbox, confidence
        
        return None, 0.0
    except Exception as e:
        print(f"[WARN] Hand detection failed: {e}")
        return None, 0.0

def detect_hand_keypoints(image, bbox=None):
    global onnx_session
    
    if onnx_session is None:
        return None
    
    h, w = image.shape[:2]
    
    if bbox is not None:
        x_min = max(0, bbox['x_min'])
        y_min = max(0, bbox['y_min'])
        x_max = min(w, bbox['x_max'])
        y_max = min(h, bbox['y_max'])
        
        bbox_w = x_max - x_min
        bbox_h = y_max - y_min
        
        padding = max(bbox_w, bbox_h) * 0.3
        x_min = max(0, x_min - padding)
        y_min = max(0, y_min - padding)
        x_max = min(w, x_max + padding)
        y_max = min(h, y_max + padding)
        
        crop_w = x_max - x_min
        crop_h = y_max - y_min
        crop_size = max(crop_w, crop_h)
        
        center_x = (x_min + x_max) / 2
        center_y = (y_min + y_max) / 2
        
        start_x = int(max(0, center_x - crop_size / 2))
        start_y = int(max(0, center_y - crop_size / 2))
        end_x = int(min(w, center_x + crop_size / 2))
        end_y = int(min(h, center_y + crop_size / 2))
        
        actual_size = max(end_x - start_x, end_y - start_y)
        
        cropped = image[start_y:end_y, start_x:end_x]
        
        if cropped.size == 0:
            cropped = center_crop_to_square(image)
            size = min(h, w)
            start_x = (w - size) // 2
            start_y = (h - size) // 2
            actual_size = size
    else:
        cropped = center_crop_to_square(image)
        size = min(h, w)
        start_x = (w - size) // 2
        start_y = (h - size) // 2
        actual_size = size
    
    img_resized = cv2.resize(cropped, (IMG_SIZE, IMG_SIZE), interpolation=cv2.INTER_LINEAR)
    img_ndarray = cv2.dnn.blobFromImage(img_resized, 1.0/255.0, (IMG_SIZE, IMG_SIZE), swapRB=False, crop=False)
    
    input_name = onnx_session.get_inputs()[0].name
    output = onnx_session.run(None, {input_name: img_ndarray})[0][0]
    
    keypoints = []
    for i in range(int(output.shape[0] / 2)):
        x = float(output[i * 2]) * actual_size + start_x
        y = float(output[i * 2 + 1]) * actual_size + start_y
        keypoints.append((x, y))
    
    return keypoints

def recognize_gesture_from_keypoints(keypoints):
    if keypoints is None or len(keypoints) < 21:
        return "unknown", 0.0
    
    pts = {str(i): {'x': keypoints[i][0], 'y': keypoints[i][1]} for i in range(21)}
    
    wrist = pts['0']
    middle_base = pts['9']
    palm_size = ((wrist['x'] - middle_base['x']) ** 2 + (wrist['y'] - middle_base['y']) ** 2) ** 0.5
    if palm_size == 0:
        palm_size = 1
    
    thumb_dist = ((pts['4']['x'] - wrist['x']) ** 2 + (pts['4']['y'] - wrist['y']) ** 2) ** 0.5 / palm_size
    index_dist = ((pts['8']['x'] - wrist['x']) ** 2 + (pts['8']['y'] - wrist['y']) ** 2) ** 0.5 / palm_size
    middle_dist = ((pts['12']['x'] - wrist['x']) ** 2 + (pts['12']['y'] - wrist['y']) ** 2) ** 0.5 / palm_size
    ring_dist = ((pts['16']['x'] - wrist['x']) ** 2 + (pts['16']['y'] - wrist['y']) ** 2) ** 0.5 / palm_size
    pinky_dist = ((pts['20']['x'] - wrist['x']) ** 2 + (pts['20']['y'] - wrist['y']) ** 2) ** 0.5 / palm_size
    
    ratio_list = [thumb_dist, index_dist, middle_dist, ring_dist, pinky_dist]
    
    thr_ratio = [1.3, 1.5, 1.3, 1.1]
    thr_ratio_thumb = 1.0
    
    finger_states = []
    if ratio_list[0] > thr_ratio_thumb:
        finger_states.append(1)
    else:
        finger_states.append(0)
    
    for i in range(1, 5):
        if ratio_list[i] > thr_ratio[i - 1]:
            finger_states.append(1)
        else:
            finger_states.append(0)
    
    if (ratio_list[0] <= 1.4 and ratio_list[1] <= 1.2 and 
        ratio_list[2] <= 1.0 and ratio_list[3] <= 0.9 and ratio_list[4] <= 0.9):
        return "0", 0.9
    
    four_fingers_extended = all(finger_states[1:5])
    
    if four_fingers_extended:
        if finger_states[0] == 1:
            return "5", 0.9
        else:
            return "4", 0.85
    elif finger_states == [0, 1, 0, 0, 0]:
        return "1", 0.85
    elif finger_states == [0, 1, 1, 0, 0]:
        return "2", 0.85
    elif finger_states == [0, 1, 1, 1, 0]:
        return "3", 0.85
    
    return "unknown", 0.3

def detect_gesture(image):
    hand_bbox, hand_confidence = detect_hand_presence(image)
    
    if hand_bbox is None:
        return {
            "type": "none",
            "confidence": 0.0,
            "keypoints_count": 0,
            "keypoints": [],
            "hand_detected": False
        }
    
    keypoints = detect_hand_keypoints(image, bbox=hand_bbox)
    
    if keypoints is None or len(keypoints) < 21:
        return {
            "type": "none",
            "confidence": 0.0,
            "keypoints_count": 0 if keypoints is None else len(keypoints),
            "keypoints": [],
            "hand_detected": True,
            "hand_bbox": hand_bbox
        }
    
    gesture_type, confidence = recognize_gesture_from_keypoints(keypoints)
    
    return {
        "type": gesture_type,
        "confidence": float(confidence),
        "keypoints_count": len(keypoints),
        "keypoints": [(float(x), float(y)) for x, y in keypoints],
        "hand_detected": True,
        "hand_bbox": hand_bbox,
        "hand_confidence": float(hand_confidence)
    }

def draw_gesture_visualization(image, result):
    vis_image = image.copy()
    
    hand_detected = result.get('hand_detected', False)
    
    if not hand_detected:
        cv2.putText(vis_image, "No hand detected", (10, 30), 
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2)
        cv2.putText(vis_image, "Please show your hand", (10, 60), 
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (128, 128, 128), 2)
        return vis_image
    
    cv2.putText(vis_image, f"Gesture: {result['type']}", (10, 30), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
    cv2.putText(vis_image, f"Confidence: {result['confidence']:.2f}", (10, 60), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    
    hand_bbox = result.get('hand_bbox')
    if hand_bbox:
        x_min, y_min = int(hand_bbox['x_min']), int(hand_bbox['y_min'])
        x_max, y_max = int(hand_bbox['x_max']), int(hand_bbox['y_max'])
        cv2.rectangle(vis_image, (x_min, y_min), (x_max, y_max), (255, 0, 0), 2)
    
    keypoints = result.get('keypoints', [])
    if keypoints and len(keypoints) >= 21:
        for i, (x, y) in enumerate(keypoints):
            cv2.circle(vis_image, (int(x), int(y)), 5, (0, 0, 255), -1)
        
        for start_idx, end_idx in FINGER_CONNECTIONS:
            if start_idx < len(keypoints) and end_idx < len(keypoints):
                start_point = (int(keypoints[start_idx][0]), int(keypoints[start_idx][1]))
                end_point = (int(keypoints[end_idx][0]), int(keypoints[end_idx][1]))
                cv2.line(vis_image, start_point, end_point, (0, 255, 255), 2)
    
    return vis_image

@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "gesture_detector",
        "version": "3.0.0",
        "method": "handpose_x_ONNX"
    })

@app.route('/detect', methods=['POST'])
def detect():
    start_time = time.time()
    
    try:
        data = request.json
        image_base64 = data.get('image')
        
        if not image_base64:
            return jsonify({
                "success": False,
                "error": "No image provided"
            }), 400
        
        image = decode_image(image_base64)
        if image is None:
            return jsonify({
                "success": False,
                "error": "Failed to decode image"
            }), 400
        
        result = detect_gesture(image)
        
        processing_time = time.time() - start_time
        
        return jsonify({
            "success": True,
            "result": result,
            "processing_time": processing_time
        })
    except Exception as e:
        print(f"[ERROR] detect exception: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({
            "success": False,
            "error": str(e)
        }), 500

@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    start_time = time.time()
    
    try:
        data = request.json
        image_base64 = data.get('image')
        
        if not image_base64:
            return jsonify({
                "success": False,
                "error": "No image provided"
            }), 400
        
        image = decode_image(image_base64)
        if image is None:
            return jsonify({
                "success": False,
                "error": "Failed to decode image"
            }), 400
        
        result = detect_gesture(image)
        
        vis_image = draw_gesture_visualization(image, result)
        
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
        print(f"[ERROR] detect_visualize exception: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({
            "success": False,
            "error": str(e)
        }), 500

load_onnx_model()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 8002))
    app.run(host='0.0.0.0', port=port)
