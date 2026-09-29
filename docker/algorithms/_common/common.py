"""算法服务公共工具模块。

被各算法服务（vlm-game-detector / vlm-visual-qa 等）复用，
避免 decode/encode/中文字体渲染逻辑在多个服务中重复。
"""

import base64
import os

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def decode_image_b64(image_b64):
    """将 base64 编码的 JPEG/图像解码为 BGR ndarray。

    Args:
        image_b64: base64 字符串

    Returns:
        解码成功返回 BGR 图像，失败返回 None。
    """
    try:
        image_bytes = base64.b64decode(image_b64)
        nparr = np.frombuffer(image_bytes, np.uint8)
        return cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    except Exception:
        return None


def encode_b64(frame, quality=80):
    """将 BGR ndarray 编码为 JPEG 并转为 base64 字符串。"""
    _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return base64.b64encode(buffer).decode('utf-8')


def find_chinese_font(size=18):
    """查找可用的中文字体"""
    candidates = [
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                continue
    return ImageFont.load_default()


def put_chinese_text(img, text, pos, font, color=(255, 255, 255)):
    """在 OpenCV 图像上绘制中文文本"""
    pil_img = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(pil_img)
    draw.text(pos, text, font=font, fill=color[::-1])  # BGR -> RGB
    return cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)
