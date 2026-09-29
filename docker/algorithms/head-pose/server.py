from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import mediapipe as mp
import math

app = Flask(__name__)

mp_face_detection = mp.solutions.face_detection
face_detection = mp_face_detection.FaceDetection(
    model_selection=1,
    min_detection_confidence=0.7
)

WHENET_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'WHENet.h5')
whenet_model = None

def load_whenet():
    global whenet_model
    if whenet_model is None:
        try:
            from whenet import WHENet
            whenet_model = WHENet(WHENET_MODEL_PATH)
            print("[INFO] WHENet model loaded successfully")
        except Exception as e:
            print(f"[WARN] Failed to load WHENet model: {e}, falling back to MediaPipe PnP")
            whenet_model = None

def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image

MODEL_POINTS = np.array([
    (0.0, 0.0, 0.0),
    (0.0, -63.6, -12.5),
    (-43.3, 32.7, -26.0),
    (43.3, 32.7, -26.0),
    (-28.9, -28.9, -24.1),
    (28.9, -28.9, -24.1),
], dtype=np.float64)

mp_face_mesh = mp.solutions.face_mesh
face_mesh = mp_face_mesh.FaceMesh(
    max_num_faces=1,
    refine_landmarks=True,
    min_detection_confidence=0.7,
    min_tracking_confidence=0.7
)

def get_camera_matrix(size):
    return np.array([[size, 0, size/2.0], [0, size, size/2.0], [0, 0, 1]], dtype=np.float64)

def rotation_matrix_to_euler_angles(R):
    sy = math.sqrt(R[0,0] * R[0,0] + R[1,0] * R[1,0])
    singular = sy < 1e-6
    if not singular:
        x, y, z = math.atan2(R[2,1], R[2,2]), math.atan2(-R[2,0], sy), math.atan2(R[1,0], R[0,0])
    else:
        x, y, z = math.atan2(-R[1,2], R[1,1]), math.atan2(-R[2,0], sy), 0
    return math.degrees(x), math.degrees(y), math.degrees(z)

def detect_head_pose_whenet(image):
    global whenet_model
    h, w = image.shape[:2]
    
    results = face_detection.process(cv2.cvtColor(image, cv2.COLOR_BGR2RGB))
    
    if not results.detections:
        return detect_head_pose_mediapipe(image)
    
    detection = results.detections[0]
    bbox = detection.location_data.relative_bounding_box
    x_min = max(0, int(bbox.xmin * w))
    y_min = max(0, int(bbox.ymin * h))
    x_max = min(w, int((bbox.xmin + bbox.width) * w))
    y_max = min(h, int((bbox.ymin + bbox.height) * h))
    
    if whenet_model is not None:
        try:
            face_img = image[y_min:y_max, x_min:x_max]
            if face_img.size == 0:
                return detect_head_pose_mediapipe(image)
            
            face_rgb = cv2.cvtColor(face_img, cv2.COLOR_BGR2RGB)
            face_resized = cv2.resize(face_rgb, (224, 224))
            face_input = np.expand_dims(face_resized, axis=0)
            
            yaw, pitch, roll = whenet_model.get_angle(face_input)
            yaw = float(yaw[0])
            pitch = float(pitch[0])
            roll = float(roll[0])
            
            face_x = yaw * 10
            face_y = pitch * 10
            
            return {
                "face_x": float(face_x),
                "face_y": float(face_y),
                "yaw": float(yaw),
                "pitch": float(pitch),
                "roll": float(roll),
                "method": "WHENet"
            }
        except Exception as e:
            print(f"[WARN] WHENet inference failed: {e}, falling back to MediaPipe")
            return detect_head_pose_mediapipe(image)
    
    return detect_head_pose_mediapipe(image)

def detect_head_pose_mediapipe(image):
    h, w = image.shape[:2]
    size = max(h, w)
    
    pad_h, pad_w = (size-h)//2, (size-w)//2
    padded = cv2.copyMakeBorder(image, pad_h, size-h-pad_h, pad_w, size-w-pad_w, cv2.BORDER_CONSTANT, value=[0,0,0])
    
    rgb = cv2.cvtColor(padded, cv2.COLOR_BGR2RGB)
    results = face_mesh.process(rgb)
    
    face_x = 0.0
    face_y = 0.0
    yaw = 0.0
    pitch = 0.0
    roll = 0.0
    
    if results.multi_face_landmarks:
        landmarks = results.multi_face_landmarks[0].landmark
        image_points = []
        
        for idx in [1, 199, 33, 263, 61, 291]:
            image_points.append([landmarks[idx].x * size, landmarks[idx].y * size])
        
        image_points = np.array(image_points, dtype=np.float64)
        success, rot_vec, trans_vec = cv2.solvePnP(MODEL_POINTS, image_points, get_camera_matrix(size), np.zeros((4,1)))
        
        if success:
            rot_mat, _ = cv2.Rodrigues(rot_vec)
            pitch, yaw, roll = rotation_matrix_to_euler_angles(rot_mat)
            
            face_x = yaw * 10
            face_y = pitch * 10
    
    return {
        "face_x": float(face_x),
        "face_y": float(face_y),
        "yaw": float(yaw),
        "pitch": float(pitch),
        "roll": float(roll),
        "method": "MediaPipe"
    }

@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "head_pose_detector",
        "version": "3.0.0",
        "method": "WHENet+MediaPipe"
    })

def draw_head_pose_visualization(image, result):
    vis_image = image.copy()
    h, w = vis_image.shape[:2]
    
    cv2.putText(vis_image, f"Method: {result['method']}", (10, 30), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
    cv2.putText(vis_image, f"Yaw: {result['yaw']:.1f}", (10, 60), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    cv2.putText(vis_image, f"Pitch: {result['pitch']:.1f}", (10, 90), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    cv2.putText(vis_image, f"Roll: {result['roll']:.1f}", (10, 120), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    
    center_x, center_y = w // 2, h // 2
    axis_length = min(w, h) // 4
    
    yaw_rad = math.radians(result['yaw'])
    pitch_rad = math.radians(result['pitch'])
    
    end_x = int(center_x + axis_length * math.sin(yaw_rad))
    end_y = int(center_y - axis_length * math.sin(pitch_rad))
    
    cv2.circle(vis_image, (center_x, center_y), 10, (0, 0, 255), -1)
    cv2.line(vis_image, (center_x, center_y), (end_x, end_y), (0, 255, 0), 3)
    cv2.circle(vis_image, (end_x, end_y), 8, (0, 255, 0), -1)
    
    return vis_image

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
        result = detect_head_pose_whenet(image)
        
        processing_time = time.time() - start_time
        
        return jsonify({
            "success": True,
            "result": result,
            "processing_time": processing_time
        })
    except Exception as e:
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
        result = detect_head_pose_whenet(image)
        
        vis_image = draw_head_pose_visualization(image, result)
        
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

load_whenet()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 8001))
    app.run(host='0.0.0.0', port=port)
