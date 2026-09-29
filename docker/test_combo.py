import requests
import base64
import cv2
import json
import time

# 读取一帧测试图
cap = cv2.VideoCapture(0)
ret, frame = cap.read()
cap.release()

if not ret:
    print("Failed to capture frame")
    exit()

_, buffer = cv2.imencode('.jpg', frame)
img_b64 = base64.b64encode(buffer).decode('utf-8')

# 调用gesture-mantis API
resp = requests.post('http://127.0.0.1:9009/detect', json={'image': img_b64}, timeout=5)
data = resp.json()

print(json.dumps(data, indent=2, ensure_ascii=False))
print()
print(f"gesture: {data.get('result', {}).get('gesture')}")
print(f"combo_gesture: {data.get('result', {}).get('combo_gesture')}")
print(f"hand_detected: {data.get('result', {}).get('hand_detected')}")
