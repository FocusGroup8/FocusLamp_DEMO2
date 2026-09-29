import sys
from loguru import logger

_configured = False


def setup_logging(log_level: str = "INFO"):
    global _configured
    if not _configured:
        log_format = "<green>{time:YY-MM-DD HH:mm:ss}</green> - <level>{level}</level> - <light-green>{message}</light-green>"
        logger.remove()
        logger.add(sys.stdout, format=log_format, level=log_level)
        _configured = True
    return logger
