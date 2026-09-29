from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import threading
import mediapipe as mp

app = Flask(__name__)

GESTURE_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gesture_recognizer.task')

gesture_recognizer = None
gesture_lock = threading.Lock()

last_gesture_category = None
last_gesture_score = 0.0
last_hand_landmarks = None
last_hand_detected = False


# ========== 组合手势检测 ==========
_INVALID_GESTURES = {None, "None"}

COMBO_GESTURES = {
    ("Closed_Fist", "Open_Palm"): "Open",
    ("Open_Palm", "Closed_Fist"): "Close",
    ("Closed_Fist", "Thumb_Up"): "VolumeUp",
    ("Closed_Fist", "Thumb_Down"): "VolumeDown",
}

COMBO_GESTURE_COLORS = {
    "Open": (0, 255, 0),
    "Close": (0, 0, 255),
    "VolumeUp": (0, 200, 255),
    "VolumeDown": (255, 100, 0),
}

EMOTION_GESTURE_COMBOS = {
    ("Happiness", "Open_Palm"): "Welcome",
    ("Happiness", "Thumb_Up"): "Good",
}

EMOTION_COMBO_COLORS = {
    "Welcome": (0, 255, 200),
    "Good": (0, 255, 128),
}


class ComboGestureDetector:
    def __init__(self):
        self.last_valid_gesture = None

    def update(self, current_gesture):
        if current_gesture in _INVALID_GESTURES:
            return None

        if self.last_valid_gesture is None:
            self.last_valid_gesture = current_gesture
            return None

        if current_gesture == self.last_valid_gesture:
            return None

        combo_name = COMBO_GESTURES.get((self.last_valid_gesture, current_gesture))
        self.last_valid_gesture = current_gesture
        return combo_name

    def reset(self):
        self.last_valid_gesture = None


combo_detector = ComboGestureDetector()
last_combo_gesture = None


def load_models():
    global gesture_recognizer
    if gesture_recognizer is None:
        try:
            BaseOptions = mp.tasks.BaseOptions
            GestureRecognizer = mp.tasks.vision.GestureRecognizer
            GestureRecognizerOptions = mp.tasks.vision.GestureRecognizerOptions
            VisionRunningMode = mp.tasks.vision.RunningMode

            options = GestureRecognizerOptions(
                base_options=BaseOptions(model_asset_path=GESTURE_MODEL_PATH),
                running_mode=VisionRunningMode.IMAGE,
                num_hands=1,
                min_hand_detection_confidence=0.5,
                min_hand_presence_confidence=0.5,
                min_tracking_confidence=0.5,
            )
            gesture_recognizer = GestureRecognizer.create_from_options(options)
            print("[INFO] MediaPipe GestureRecognizer loaded (IMAGE mode)", flush=True)
        except Exception as e:
            print(f"[ERROR] Failed to load GestureRecognizer: {e}", flush=True)


def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image


def detect_gesture(image):
    global last_gesture_category, last_gesture_score, last_hand_landmarks, last_hand_detected, last_combo_gesture

    image = cv2.flip(image, 1)

    if gesture_recognizer is None:
        return {
            'gesture': 'None',
            'confidence': 0.0,
            'hand_detected': False,
            'combo_gesture': None,
        }

    try:
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = gesture_recognizer.recognize(mp_image)

        gesture_category = 'None'
        gesture_score = 0.0
        hand_detected = False
        hand_landmarks = None

        if result.gestures and len(result.gestures) > 0:
            gestures = result.gestures[0]
            if gestures:
                gesture_category = gestures[0].category_name
                gesture_score = float(gestures[0].score)

        if result.hand_landmarks and len(result.hand_landmarks) > 0:
            hand_detected = True
            hand_landmarks = [
                {'x': lm.x, 'y': lm.y, 'z': lm.z}
                for lm in result.hand_landmarks[0]
            ]

        # 组合手势检测
        combo_result = combo_detector.update(gesture_category)
        if combo_result is not None:
            last_combo_gesture = combo_result
            print(f"[COMBO] {combo_detector.last_valid_gesture} <- {gesture_category} => Combo: {combo_result}", flush=True)

        with gesture_lock:
            last_gesture_category = gesture_category
            last_gesture_score = gesture_score
            last_hand_landmarks = hand_landmarks
            last_hand_detected = hand_detected

        return {
            'gesture': gesture_category,
            'confidence': round(gesture_score, 4),
            'hand_detected': hand_detected,
            'hand_landmarks_count': len(hand_landmarks) if hand_landmarks else 0,
            'hand_landmarks': hand_landmarks,
            'combo_gesture': last_combo_gesture,
        }
    except Exception as e:
        print(f"[WARN] GestureRecognizer detect failed: {e}", flush=True)
        return {
            'gesture': 'None',
            'confidence': 0.0,
            'hand_detected': False,
            'combo_gesture': None,
        }


def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
    (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
    cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2), 
                 (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
    cv2.putText(img, text, pos, font, scale, color, thickness)

def draw_gesture_visualization(image, result, current_emotion=None):
    vis_image = cv2.flip(image.copy(), 1)

    gesture = result.get('gesture', 'None')
    confidence = result.get('confidence', 0.0)
    hand_detected = result.get('hand_detected', False)
    combo_gesture = result.get('combo_gesture')

    if not hand_detected:
        draw_text_with_bg(vis_image, "No hand detected", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (128, 128, 128), 2)
        if current_emotion:
            draw_text_with_bg(vis_image, f"Emotion: {current_emotion}", (10, 60),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 2)
        return vis_image

    gesture_colors = {
        'Closed_Fist': (0, 0, 255),
        'Open_Palm': (0, 255, 0),
        'Pointing_Up': (0, 255, 255),
        'Thumb_Down': (0, 100, 255),
        'Thumb_Up': (0, 200, 0),
        'Victory': (255, 0, 0),
        'ILoveYou': (255, 0, 255),
        'None': (128, 128, 128),
    }

    color = gesture_colors.get(gesture, (255, 255, 255))

    y_offset = 30
    draw_text_with_bg(vis_image, f"Gesture: {gesture}", (10, y_offset),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
    y_offset += 30
    draw_text_with_bg(vis_image, f"Confidence: {confidence:.2f}", (10, y_offset),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    y_offset += 25

    if combo_gesture:
        combo_color = COMBO_GESTURE_COLORS.get(combo_gesture, (255, 255, 255))
        draw_text_with_bg(vis_image, f"Combo: {combo_gesture}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, combo_color, 2)
        y_offset += 30

    # 情绪+手势联动
    if current_emotion and gesture not in ('None', None):
        emotion_cap = current_emotion.capitalize()
        emotion_combo_key = (emotion_cap, gesture)
        emotion_combo_name = EMOTION_GESTURE_COMBOS.get(emotion_combo_key)
        if emotion_combo_name:
            ec_color = EMOTION_COMBO_COLORS.get(emotion_combo_name, (255, 255, 255))
            draw_text_with_bg(vis_image, f"EmotionCombo: {emotion_combo_name}", (10, y_offset),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, ec_color, 2)
            y_offset += 30

    if current_emotion:
        draw_text_with_bg(vis_image, f"Emotion: {current_emotion}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 2)

    with gesture_lock:
        landmarks = last_hand_landmarks

    if landmarks:
        h, w = vis_image.shape[:2]
        HAND_CONNECTIONS = [
            (0, 1), (1, 2), (2, 3), (3, 4),
            (0, 5), (5, 6), (6, 7), (7, 8),
            (0, 9), (9, 10), (10, 11), (11, 12),
            (0, 13), (13, 14), (14, 15), (15, 16),
            (0, 17), (17, 18), (18, 19), (19, 20),
            (5, 9), (9, 13), (13, 17),
        ]
        points = []
        for lm in landmarks:
            cx, cy = int(lm['x'] * w), int(lm['y'] * h)
            points.append((cx, cy))
            cv2.circle(vis_image, (cx, cy), 4, (0, 255, 255), -1)
            cv2.circle(vis_image, (cx, cy), 5, (0, 0, 0), 1)
        for i, j in HAND_CONNECTIONS:
            if i < len(points) and j < len(points):
                cv2.line(vis_image, points[i], points[j], (255, 128, 0), 2)

    return vis_image


@app.route('/health', methods=['GET'])
def health():
    with gesture_lock:
        gesture = last_gesture_category
        hand = last_hand_detected
    return jsonify({
        "status": "ok",
        "algorithm": "gesture_mantis",
        "version": "1.0.0",
        "method": "MediaPipe_GestureRecognizer",
        "last_gesture": gesture,
        "hand_detected": hand,
    })


@app.route('/reset', methods=['POST'])
def reset():
    global last_gesture_category, last_gesture_score, last_hand_landmarks, last_hand_detected, last_combo_gesture
    with gesture_lock:
        last_gesture_category = None
        last_gesture_score = 0.0
        last_hand_landmarks = None
        last_hand_detected = False
        last_combo_gesture = None
    combo_detector.reset()
    return jsonify({
        "success": True,
        "message": "Gesture state reset",
    })


@app.route('/detect', methods=['POST'])
def detect():
    start_time_req = time.time()

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
            }), 400

        result = detect_gesture(image)

        processing_time = time.time() - start_time_req

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
    start_time_req = time.time()

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
            }), 400

        result = detect_gesture(image)

        current_emotion = data.get('current_emotion')
        vis_image = draw_gesture_visualization(image, result, current_emotion)

        _, buffer = cv2.imencode('.jpg', vis_image, [cv2.IMWRITE_JPEG_QUALITY, 80])
        vis_base64 = base64.b64encode(buffer).decode('utf-8')

        processing_time = time.time() - start_time_req

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


load_models()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 9009))
    app.run(host='0.0.0.0', port=port)
