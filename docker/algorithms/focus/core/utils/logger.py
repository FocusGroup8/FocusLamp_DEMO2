import logging
import sys

_logger = None

def setup_logging():
    global _logger
    if _logger is not None:
        return _logger
    _logger = logging.getLogger("focus")
    _logger.setLevel(logging.DEBUG)
    handler = logging.StreamHandler(sys.stdout)
    handler.setLevel(logging.DEBUG)
    formatter = logging.Formatter(
        '%(asctime)s [%(name)s] %(levelname)s - %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    handler.setFormatter(formatter)
    _logger.addHandler(handler)
    return _logger
