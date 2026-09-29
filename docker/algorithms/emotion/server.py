from flask import Flask, request, jsonify
import cv2
import numpy as np
import base64
import time
import os
import yaml

app = Flask(__name__)

CONFIG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config')
emotion_detector = None

EMOTION_TO_RATING = {
    "anger": 11, "angry": 11, "contempt": 30, "disgust": 30, "fear": 20,
    "happiness": 50, "happy": 50, "neutral": 41, "sadness": 10, "sad": 10, "surprise": 40
}

def load_emotion_detector():
    global emotion_detector
    if emotion_detector is None:
        try:
            from core.providers.emotion_detector import EmotionDetectorProvider
            config_path = os.path.join(CONFIG_DIR, 'emotion_detector.yaml')
            emotion_detector = EmotionDetectorProvider(config_path=config_path)
            print("[INFO] EmotionDetectorProvider loaded successfully", flush=True)
        except Exception as e:
            print(f"[ERROR] Failed to load EmotionDetectorProvider: {e}", flush=True)
            raise e

def decode_image(image_base64):
    img_data = base64.b64decode(image_base64)
    nparr = np.frombuffer(img_data, np.uint8)
    image = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    return image

def detect_emotion(image):
    global emotion_detector
    
    if emotion_detector is None:
        return {
            "type": "no_model",
            "confidence": 0.0,
            "rating": 0,
            "face_bbox": None
        }
    
    try:
        result = emotion_detector.detect(image)
        
        if result is None:
            return {
                "type": "no_face",
                "confidence": 0.0,
                "rating": 0,
                "face_bbox": None
            }
        
        emotion_name = result.get('emotion', 'neutral')
        confidence = result.get('confidence', 0.0)
        rating = EMOTION_TO_RATING.get(emotion_name, 41)
        
        return {
            "type": emotion_name,
            "confidence": float(confidence),
            "rating": rating,
            "face_bbox": None
        }
    
    except Exception as e:
        print(f"[WARN] Emotion inference failed: {e}")
        return {
            "type": "error",
            "confidence": 0.0,
            "rating": 0,
            "error": str(e),
            "face_bbox": None
        }

def draw_emotion_visualization(image, result):
    vis_image = cv2.flip(image.copy(), 1)
    
    emotion_colors = {
        "happiness": (0, 255, 0),
        "sadness": (255, 100, 0),
        "anger": (0, 0, 255),
        "surprise": (0, 255, 255),
        "fear": (128, 0, 128),
        "disgust": (0, 180, 0),
        "contempt": (0, 140, 200),
        "neutral": (200, 200, 200),
        "no_face": (128, 128, 128),
        "no_model": (100, 100, 100),
        "error": (0, 0, 255)
    }
    
    emotion = result['type']
    color = emotion_colors.get(emotion, (255, 255, 255))
    
    def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
        (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
        cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2), 
                     (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
        cv2.putText(img, text, pos, font, scale, color, thickness)
    
    draw_text_with_bg(vis_image, f"Emotion: {emotion}", (10, 30), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
    draw_text_with_bg(vis_image, f"Confidence: {result['confidence']:.2f}", (10, 60), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    draw_text_with_bg(vis_image, f"Rating: {result['rating']}", (10, 90), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
    
    return vis_image

@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "algorithm": "emotion_detector",
        "version": "4.0.0",
        "method": "EmotiEffLib_MTCNN"
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
        result = detect_emotion(image)
        
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
        result = detect_emotion(image)
        
        vis_image = draw_emotion_visualization(image, result)
        
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

load_emotion_detector()

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 9004))
    app.run(host='0.0.0.0', port=port)
