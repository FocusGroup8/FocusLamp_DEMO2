from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from core.providers.engage_detector.mantis import EngageDetectorProvider
from core.providers.eye_detector.mediapipe import EyeDetectorProvider
from core.providers.face_detector.mediapipe import FaceDetectorProvider
from core.providers.gesture_detector.mediapipe import GestureDetectorProvider
from core.manager.pomodoro_manager import PomodoroManager

app = Flask(__name__)

CONFIG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config')

engage_detector = None
eye_detector = None
face_detector = None
gesture_detector = None
pomodoro_manager = None
processing_lock = threading.Lock()


def load_models():
    global engage_detector, eye_detector, face_detector, gesture_detector, pomodoro_manager

    window_duration = int(os.environ.get('WINDOW_DURATION', 90))

    engage_detector = EngageDetectorProvider(config_path=os.path.join(CONFIG_DIR, 'engage_detector.yaml'))

    try:
        eye_detector = EyeDetectorProvider(config_path=os.path.join(CONFIG_DIR, 'eye_detector.yaml'))
        print("[INFO] EyeDetector loaded successfully", flush=True)
    except Exception as e:
        print(f"[WARN] EyeDetector load failed: {e}", flush=True)
        eye_detector = None

    try:
        face_detector = FaceDetectorProvider(config_path=os.path.join(CONFIG_DIR, 'face_detector.yaml'))
        print("[INFO] FaceDetector loaded successfully", flush=True)
    except Exception as e:
        print(f"[WARN] FaceDetector load failed: {e}", flush=True)
        face_detector = None

    try:
        gesture_detector = GestureDetectorProvider(config_path=os.path.join(CONFIG_DIR, 'gesture_detector.yaml'))
        print("[INFO] GestureDetector loaded successfully", flush=True)
    except Exception as e:
        print(f"[WARN] GestureDetector load failed: {e}", flush=True)
        gesture_detector = None

    pomodoro_manager = PomodoroManager(window_duration=window_duration)

    print("[INFO] EngageDetector + EyeDetector + FaceDetector + GestureDetector + PomodoroManager loaded successfully", flush=True)


def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image


def process_video_frame(frame):
    frame = cv2.flip(frame, 1)
    frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

    engage_detector.run(frame_rgb)
    engage_detect_result = engage_detector.get_detect_result()

    if engage_detect_result is not None:
        pomodoro_manager.update(engage_level=engage_detect_result['engage_level'])

    display_engage = engage_detect_result if engage_detect_result is not None else engage_detector.last_inference_result
    if display_engage is None:
        display_engage = {"engage_level": "Unknown", "engage_level_name": "Unknown"}

    result = {
        'engage_detect_result': display_engage,
    }

    if pomodoro_manager.last_focus_result is not None:
        result['focus_level'] = pomodoro_manager.last_focus_result['focus_level']
        result['focus_level_name'] = pomodoro_manager.last_focus_result['focus_level_name']
        result['focus_score'] = pomodoro_manager.last_focus_result['focus_score']
        result['engage_level_count'] = pomodoro_manager.last_focus_result['engage_level_count']

    if eye_detector is not None:
        eye_detector.run(frame_rgb)
        eye_result = eye_detector.get_detect_result()
        if eye_result is not None:
            result['eye_detect_result'] = eye_result

    if face_detector is not None:
        face_detector.run(frame_rgb)
        face_result = face_detector.get_detect_result()
        if face_result is not None:
            result['face_detect_result'] = face_result

    if gesture_detector is not None:
        gesture_detector.run(frame_rgb)
        gesture_result = gesture_detector.get_detect_result()
        if gesture_result is not None:
            result['gesture_detect_result'] = gesture_result

    return result


def draw_focus_visualization(image, result):
    vis_image = cv2.flip(image.copy(), 1)

    engage_result = result.get('engage_detect_result', {})
    engage_level_name = engage_result.get('engage_level_name', 'Unknown') if isinstance(engage_result, dict) else 'Unknown'

    engage_colors = {
        'Highly-Engaged': (0, 255, 0),
        'Engaged': (0, 200, 0),
        'Barely-engaged': (0, 255, 255),
        'Not-Engaged': (0, 0, 255),
        'Unknown': (128, 128, 128),
    }

    focus_colors = {
        'HighFocused': (0, 255, 0),
        'MediumFocused': (0, 200, 100),
        'LowFocused': (0, 200, 200),
        'Neutral': (200, 200, 0),
        'LowDistracted': (0, 165, 255),
        'MediumDistracted': (0, 100, 255),
        'HighDistracted': (0, 0, 255),
    }

    def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
        (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
        cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2), 
                     (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
        cv2.putText(img, text, pos, font, scale, color, thickness)

    y_offset = 30

    color = engage_colors.get(engage_level_name, (255, 255, 255))
    draw_text_with_bg(vis_image, f"Engage: {engage_level_name}", (10, y_offset),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
    y_offset += 35

    focus_name = result.get('focus_level_name')
    if focus_name:
        focus_color = focus_colors.get(focus_name, (255, 255, 255))
        draw_text_with_bg(vis_image, f"Focus: {focus_name}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, focus_color, 2)
        y_offset += 35

    focus_score = result.get('focus_score')
    if focus_score is not None:
        draw_text_with_bg(vis_image, f"FocusScore: {focus_score:.4f}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
        y_offset += 28

    eye_result = result.get('eye_detect_result')
    if eye_result:
        h_speed = eye_result.get('horizontal_speed', 0)
        v_speed = eye_result.get('vertical_speed', 0)
        draw_text_with_bg(vis_image, f"Eye H:{h_speed:.2f} V:{v_speed:.2f}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 180, 0), 2)
        y_offset += 25

    face_result = result.get('face_detect_result')
    if face_result:
        offset_x = face_result.get('normalized_offset_x', 0)
        offset_y = face_result.get('normalized_offset_y', 0)
        draw_text_with_bg(vis_image, f"Face X:{offset_x:.2f} Y:{offset_y:.2f}", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (180, 255, 0), 2)
        y_offset += 25

    gesture_result = result.get('gesture_detect_result')
    if gesture_result:
        gesture_name = gesture_result.get('gesture', 'None')
        gesture_score = gesture_result.get('score', 0)
        draw_text_with_bg(vis_image, f"Gesture: {gesture_name} ({gesture_score:.2f})", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 200, 255), 2)
    else:
        draw_text_with_bg(vis_image, "Gesture: No hand detected", (10, y_offset),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (128, 128, 128), 2)

    return vis_image


@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "focus",
        "version": "6.0.0",
        "method": "FocusCode_Experiment_Modular",
        "accumulated_frames": len(engage_detector.features_queue),
        "required_frames": 300,
        "ready": engage_detector.last_inference_result is not None,
        "focus_level": pomodoro_manager.focus_level.value,
        "focus_level_name": pomodoro_manager.focus_level.name,
        "auxiliary_detectors": {
            "eye_detector": eye_detector is not None,
            "face_detector": face_detector is not None,
            "gesture_detector": gesture_detector is not None,
        }
    })


@app.route('/reset', methods=['POST'])
def reset():
    with engage_detector.features_lock:
        engage_detector.features_queue = []
        engage_detector.last_inference_result = None
        pomodoro_manager.full_reset()
    return jsonify({
        "success": True,
        "message": "Focus detector reset",
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

        result = process_video_frame(image)

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
    if not processing_lock.acquire(blocking=False):
        return jsonify({
            "success": False,
            "error": "Server busy, skipping frame"
        }), 503

    start_time_req = time.time()

    try:
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

            result = process_video_frame(image)

            vis_image = draw_focus_visualization(image, result)

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
    finally:
        processing_lock.release()


load_models()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 9003))
    app.run(host='0.0.0.0', port=port, threaded=True)
