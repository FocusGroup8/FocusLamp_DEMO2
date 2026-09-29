import os
import sys
from loguru import logger

def setup_logging():
    log_format = "<green>{time:YY-MM-DD HH:mm:ss}</green>[<light-blue>{extra[tag]}</light-blue>] - <level>{level}</level> - <light-green>{message}</light-green>"
    log_level = os.environ.get("LOG_LEVEL", "INFO")
    
    logger.remove()
    logger.add(sys.stdout, format=log_format, level=log_level)
    
    return logger
