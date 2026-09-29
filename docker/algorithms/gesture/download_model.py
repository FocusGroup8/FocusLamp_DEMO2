import os
import urllib.request

MODEL_URL = "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task"
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hand_landmarker.task")

def download_model():
    if os.path.exists(MODEL_PATH):
        print(f"Model already exists: {MODEL_PATH}")
        return
    
    print(f"Downloading hand_landmarker.task to {MODEL_PATH}...")
    try:
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        print("Download complete!")
    except Exception as e:
        print(f"Failed to download model: {e}")

if __name__ == "__main__":
    download_model()
