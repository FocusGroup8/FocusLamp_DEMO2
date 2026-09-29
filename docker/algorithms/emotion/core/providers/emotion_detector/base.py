import numpy as np
from abc import ABC, abstractmethod
from typing import Optional

from core.utils.settings import read_config
from core.utils.logger import setup_logging

TAG = __name__

class EmotionDetectorProviderBase(ABC):
  def __init__(self, config_path: str):
    super().__init__()
    self.logger = setup_logging()
    try:
      self.config = read_config(config_path=config_path)
    except Exception as e:
      self.logger.bind(tag=TAG).error(f'Failed to read_config to initialize EmotionDetectorProvider: {e}')
      raise e
  
  @abstractmethod
  def detect(self, image: np.ndarray) -> Optional[dict]:
    pass
