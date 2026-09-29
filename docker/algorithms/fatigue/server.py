from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import dlib
from scipy.spatial import distance as dist
import math

app = Flask(__name__)

PREDICTOR_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shape_predictor_68_face_landmarks.dat')
detector = None
predictor = None

EYE_AR_THRESH = 0.2
EYE_AR_CONSEC_FRAMES = 3
MOUTH_AR_THRESH = 0.5
MOUTH_AR_CONSEC_FRAMES = 3
HAR_THRESH = 0.3
NOD_AR_CONSEC_FRAMES = 3

COUNTER = 0
TOTAL = 0
mCOUNTER = 0
mTOTAL = 0
hCOUNTER = 0
hTOTAL = 0

(lStart, lEnd) = (42, 48)
(rStart, rEnd) = (36, 42)
(mStart, mEnd) = (48, 68)

def load_dlib():
    global detector, predictor
    if detector is None:
        try:
            detector = dlib.get_frontal_face_detector()
            predictor = dlib.shape_predictor(PREDICTOR_PATH)
            print("[INFO] dlib model loaded successfully")
        except Exception as e:
            print(f"[ERROR] Failed to load dlib model: {e}")

def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image

def eye_aspect_ratio(eye):
    A = dist.euclidean(eye[1], eye[5])
    B = dist.euclidean(eye[2], eye[4])
    C = dist.euclidean(eye[0], eye[3])
    ear = (A + B) / (2.0 * C)
    return ear

def mouth_aspect_ratio(mouth):
    A = np.linalg.norm(mouth[2] - mouth[9])
    B = np.linalg.norm(mouth[4] - mouth[7])
    C = np.linalg.norm(mouth[0] - mouth[6])
    mar = (A + B) / (2.0 * C)
    return mar

def shape_to_np(shape, dtype="int"):
    coords = np.zeros((68, 2), dtype=dtype)
    for i in range(0, 68):
        coords[i] = (shape.part(i).x, shape.part(i).y)
    return coords

def get_head_pose(shape):
    object_pts = np.float32([
        [6.825897, 6.760612, 4.402142],
        [1.330353, 7.122144, 6.903745],
        [-1.330353, 7.122144, 6.903745],
        [-6.825897, 6.760612, 4.402142],
        [5.311432, 5.485328, 3.987654],
        [1.789930, 5.393625, 4.413414],
        [-1.789930, 5.393625, 4.413414],
        [-5.311432, 5.485328, 3.987654],
        [2.005628, 1.409845, 6.165652],
        [-2.005628, 1.409845, 6.165652],
        [2.774015, -2.080775, 5.048531],
        [-2.774015, -2.080775, 5.048531],
        [0.000000, -3.116408, 6.097667],
        [0.000000, -7.415691, 4.070434]
    ])
    
    K = [6.5308391993466671e+002, 0.0, 3.1950000000000000e+002,
         0.0, 6.5308391993466671e+002, 2.3950000000000000e+002,
         0.0, 0.0, 1.0]
    D = [7.0834633684407095e-002, 6.9140193737175351e-002, 0.0, 0.0, -1.3073460323689292e+000]
    
    cam_matrix = np.array(K).reshape(3, 3).astype(np.float32)
    dist_coeffs = np.array(D).reshape(5, 1).astype(np.float32)
    
    image_pts = np.float32([shape[17], shape[21], shape[22], shape[26], shape[36],
                            shape[39], shape[42], shape[45], shape[31], shape[35],
                            shape[48], shape[54], shape[57], shape[8]])
    
    _, rotation_vec, translation_vec = cv2.solvePnP(object_pts, image_pts, cam_matrix, dist_coeffs)
    rotation_mat, _ = cv2.Rodrigues(rotation_vec)
    pose_mat = cv2.hconcat((rotation_mat, translation_vec))
    _, _, _, _, _, _, euler_angle = cv2.decomposeProjectionMatrix(pose_mat)
    
    return euler_angle

def detect_fatigue(image):
    global COUNTER, TOTAL, mCOUNTER, mTOTAL, hCOUNTER, hTOTAL
    
    image = cv2.flip(image, 1)

    if detector is None or predictor is None:
        return {
            "rating": 0.0,
            "error": "dlib model not loaded",
            "landmarks": None
        }
    
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    faces = detector(gray, 0)
    
    ear = 0.0
    mar = 0.0
    har = 0.0
    blink_count = TOTAL
    yawn_count = mTOTAL
    nod_count = hTOTAL
    landmarks_np = None
    
    if len(faces) > 0:
        for k, d in enumerate(faces):
            shape = predictor(gray, d)
            shape = shape_to_np(shape)
            landmarks_np = shape.tolist()
            
            leftEye = shape[lStart:lEnd]
            rightEye = shape[rStart:rEnd]
            leftEAR = eye_aspect_ratio(leftEye)
            rightEAR = eye_aspect_ratio(rightEye)
            ear = (leftEAR + rightEAR) / 2.0
            
            if ear < EYE_AR_THRESH:
                COUNTER += 1
            else:
                if COUNTER >= EYE_AR_CONSEC_FRAMES:
                    TOTAL += 1
                COUNTER = 0
            
            mouth = shape[mStart:mEnd]
            mar = mouth_aspect_ratio(mouth)
            
            if mar > MOUTH_AR_THRESH:
                mCOUNTER += 1
            else:
                if mCOUNTER >= MOUTH_AR_CONSEC_FRAMES:
                    mTOTAL += 1
                mCOUNTER = 0
            
            try:
                euler_angle = get_head_pose(shape)
                har = euler_angle[0, 0]
                
                if har > HAR_THRESH:
                    hCOUNTER += 1
                else:
                    if hCOUNTER >= NOD_AR_CONSEC_FRAMES:
                        hTOTAL += 1
                    hCOUNTER = 0
            except Exception as e:
                print(f"[WARN] Head pose estimation failed: {e}")
            
            break
    
    blink_count = TOTAL
    yawn_count = mTOTAL
    nod_count = hTOTAL
    
    if TOTAL >= 50 or mTOTAL >= 15 or hTOTAL >= 30:
        rating = 0.9
    elif ear < EYE_AR_THRESH:
        rating = 0.7
    elif mar > MOUTH_AR_THRESH:
        rating = 0.6
    elif TOTAL >= 20 or mTOTAL >= 5 or hTOTAL >= 10:
        rating = 0.5
    elif TOTAL >= 10 or mTOTAL >= 3 or hTOTAL >= 5:
        rating = 0.3
    else:
        rating = 0.1
    
    return {
        "rating": float(rating),
        "ear": float(ear),
        "mar": float(mar),
        "har": float(har),
        "blink_count": blink_count,
        "yawn_count": yawn_count,
        "nod_count": nod_count,
        "face_detected": len(faces) > 0,
        "landmarks": landmarks_np
    }

def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
    (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
    cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2), 
                 (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
    cv2.putText(img, text, pos, font, scale, color, thickness)

def draw_fatigue_visualization(image, result):
    vis_image = cv2.flip(image.copy(), 1)
    
    fatigue_color = (0, 255, 0)
    rating = result['rating']
    if rating >= 0.7:
        fatigue_color = (0, 0, 255)
    elif rating >= 0.5:
        fatigue_color = (0, 165, 255)
    elif rating >= 0.3:
        fatigue_color = (0, 255, 255)
    
    draw_text_with_bg(vis_image, f"Fatigue: {rating:.2f}", (10, 30), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, fatigue_color, 2)
    draw_text_with_bg(vis_image, f"EAR: {result['ear']:.2f}", (10, 60), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    draw_text_with_bg(vis_image, f"MAR: {result['mar']:.2f}", (10, 90), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    draw_text_with_bg(vis_image, f"Blinks: {result['blink_count']}", (10, 120), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    draw_text_with_bg(vis_image, f"Yawns: {result['yawn_count']}", (10, 150), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    
    landmarks = result.get('landmarks')
    if landmarks:
        for i, (x, y) in enumerate(landmarks):
            cv2.circle(vis_image, (int(x), int(y)), 2, (0, 255, 255), -1)
    
    return vis_image

@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "fatigue_detector",
        "version": "3.0.0",
        "method": "dlib_EAR_MAR"
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
        result = detect_fatigue(image)
        
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
        result = detect_fatigue(image)
        
        vis_image = draw_fatigue_visualization(image, result)
        
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

load_dlib()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 8005))
    app.run(host='0.0.0.0', port=port)
