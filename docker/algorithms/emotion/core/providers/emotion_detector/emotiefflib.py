import numpy as np

from typing import List, Optional
from facenet_pytorch import MTCNN
from emotiefflib.facial_analysis import EmotiEffLibRecognizer, get_model_list
from core.providers.emotion_detector.base import EmotionDetectorProviderBase

TAG = __name__

class EmotionDetectorProvider(EmotionDetectorProviderBase):
  def __init__(self, config_path: str):
    super().__init__(config_path=config_path)
    try:
      self.device = self.config.get('device', 'cpu')
      self.mtcnn = MTCNN(keep_all=False, post_process=False, min_face_size=40, device=self.device)
      model_name = 'enet_b0_8_va_mtl'
      self.recognizer = EmotiEffLibRecognizer(engine='onnx', model_name=model_name, device=self.device)
    except Exception as e:
      self.logger.bind(tag=TAG).error(f'Failed to initialize EmotionDetectorProvider: {e}')
      raise e
  
  def _recognize_emotion(self, frame: np.ndarray) -> List[np.ndarray]:
    """
    Detects faces in the given image and returns the facial images cropped from the original.
    This function reads an image, detects faces using the MTCNN face detection model, and returns a list of cropped face images.
    Args:
      frame (numpy.ndarray): The image frame in which faces need to be detected.
    Returns:
      list: A list of numpy arrays, representing a cropped face image from the original image.
    """
    def _detect_faces(frame: np.ndarray):
      bounding_boxes, probs = self.mtcnn.detect(frame, landmarks=False)
      if probs[0] is None:
        return []
      bounding_boxes = bounding_boxes[probs > 0.9]
      return bounding_boxes

    bounding_boxes = _detect_faces(frame=frame)
    facial_images = []
    for bbox in bounding_boxes:
      box = bbox.astype(int)
      x1, y1, x2, y2 = box[0:4]
      facial_images.append(frame[y1:y2, x1:x2, :])
    return facial_images

  def detect(self, image: np.ndarray) -> Optional[dict]:
    try:
      facial_images = self._recognize_emotion(frame=image)
      if len(facial_images) == 0:
        return None
      h, w, channel = np.shape(facial_images[0])
      if h == 0 or w == 0 or channel == 0:
        return None
      res = self.recognizer.predict_emotions(facial_images[0])
      emotions, probs = res
      return {
        'emotion': emotions[0].lower(),
      }
    except Exception as e:
      self.logger.bind(tag=TAG).error(f'Failed to detect emotion: {e}')
      return None
