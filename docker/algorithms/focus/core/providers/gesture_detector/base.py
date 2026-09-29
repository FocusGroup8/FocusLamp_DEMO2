from __future__ import annotations
import numpy as np
from abc import ABC, abstractmethod

from core.utils.settings import read_config
from core.utils.logger import setup_logging

TAG = __name__

class GestureDetectorProviderBase(ABC):
    def __init__(self, config_path: str):
        super().__init__()
        self.logger = setup_logging()
        try:
            self.config = read_config(config_path=config_path)
        except Exception as e:
            self.logger.error(f'Failed to read_config to initialize GestureDetectorProvider: {e}')
            raise e

    @abstractmethod
    def get_detect_result(self) -> dict | None:
        pass

    @abstractmethod
    def run(self, image: np.ndarray):
        pass

    @abstractmethod
    def close(self):
        pass
