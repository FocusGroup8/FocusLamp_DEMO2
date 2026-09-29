import yaml
import os

from utils.logger import setup_logging

TAG = __name__

_logger = setup_logging()


def read_config(config_path: str) -> dict:
    """读取 YAML 配置文件。

    Args:
        config_path: 配置文件路径

    Returns:
        dict: 配置字典

    Raises:
        FileNotFoundError: 文件不存在
        yaml.YAMLError: YAML 解析失败
    """
    with open(config_path, "r", encoding="utf-8") as file:
        return yaml.safe_load(file)


def read_config_or_default(config_path: str, default: dict | None = None) -> dict:
    """读取 YAML 配置文件，失败时返回 default（不抛异常）。

    用于可选配置文件场景，如 face_detector.yaml 不存在时使用默认值。

    Args:
        config_path: 配置文件路径
        default: 文件不存在或解析失败时的默认值（默认空字典）

    Returns:
        dict: 配置字典（失败时返回 default 或空字典）
    """
    if default is None:
        default = {}
    if not os.path.exists(config_path):
        _logger.bind(tag=TAG).warning(
            f"Config file not found: {config_path}, using default values"
        )
        return default
    try:
        with open(config_path, "r", encoding="utf-8") as file:
            result = yaml.safe_load(file)
            return result if result is not None else default
    except yaml.YAMLError as e:
        _logger.bind(tag=TAG).error(
            f"YAML parse error in {config_path}: {e}, using default values"
        )
        return default
