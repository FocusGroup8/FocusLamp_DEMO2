"""
VLM 视觉问答服务（豆包 Doubao API）

功能：
- 接收图像 + 可选的指尖归一化坐标 (x, y)
- 如果有指尖坐标，在画面上画红色圆圈标记
- 调用豆包 Responses API 描述红圈标记处的内容
- 返回文本描述 + 可视化图像

端点：
- GET  /health            健康检查
- POST /detect            仅返回检测结果
- POST /detect_visualize  返回结果 + 可视化图像
- POST /reset             重置状态
"""

import os
import io
import time
import base64
import threading
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont
from flask import Flask, request, jsonify
from openai import OpenAI
from _common.common import decode_image_b64, encode_b64, find_chinese_font, put_chinese_text

# ============ 配置 ============
API_KEY = os.getenv("ARK_API_KEY", "")  # 通过环境变量注入，勿硬编码
MODEL_ENDPOINT = os.getenv("VLM_MODEL_ENDPOINT", "doubao-seed-2-0-mini-260428")
BASE_URL = os.getenv("VLM_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3")
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "50"))
MAX_IMAGE_WIDTH = int(os.getenv("MAX_IMAGE_WIDTH", "640"))
MARKER_RADIUS = int(os.getenv("MARKER_RADIUS", "20"))
PORT = int(os.getenv("PORT", "9013"))
# ==============================

app = Flask(__name__)

# 视觉问答 Prompt
VA_PROMPT = """用户正在用食指指向画面中的某个位置，该位置已被红色圆圈标记。
请仔细观察红圈标记处的内容，用简洁的中文描述该位置是什么。

描述要求：
- 如果是文字，请读出文字内容
- 如果是物体，请描述物体名称和特征
- 如果是人，请描述其外观特征
- 如果是屏幕上的内容，请描述具体内容

请直接描述，不要加前缀："""


class VisualQAEngine:
    """视觉问答引擎：调用豆包 API"""

    def __init__(self):
        self.client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
        self.last_answer = ""
        self.last_timestamp = 0
        self.is_processing = False
        self.lock = threading.Lock()
        self.font_medium = find_chinese_font(18)
        self.font_small = find_chinese_font(14)

    def query_sync(self, frame_bgr, fingertip_norm=None):
        """同步查询：调用 API 并返回结果（在调用方线程内执行）"""
        # 缩放
        h, w = frame_bgr.shape[:2]
        if w > MAX_IMAGE_WIDTH:
            scale = MAX_IMAGE_WIDTH / w
            frame_bgr = cv2.resize(frame_bgr, (MAX_IMAGE_WIDTH, int(h * scale)))
            h, w = frame_bgr.shape[:2]

        marked = frame_bgr.copy()
        if fingertip_norm is not None:
            tip_px = (int(fingertip_norm[0] * w), int(fingertip_norm[1] * h))
            cv2.circle(marked, tip_px, MARKER_RADIUS, (0, 0, 255), 3)
            cv2.circle(marked, tip_px, MARKER_RADIUS + 5, (0, 0, 255), 2)

        # Base64 编码
        _, buffer = cv2.imencode('.jpg', marked, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
        b64_image = base64.b64encode(buffer).decode('utf-8')
        data_uri = f"data:image/jpeg;base64,{b64_image}"

        # 调用豆包 API
        response = self.client.responses.create(
            model=MODEL_ENDPOINT,
            input=[{
                "role": "user",
                "content": [
                    {"type": "input_image", "image_url": data_uri},
                    {"type": "input_text", "text": VA_PROMPT},
                ],
            }],
            extra_body={
                "thinking": {"type": "disabled"},
            },
        )

        # 解析结果
        result_text = ""
        if hasattr(response, 'output') and response.output:
            for item in response.output:
                if hasattr(item, 'content') and item.content:
                    for content_item in item.content:
                        if hasattr(content_item, 'text'):
                            result_text += content_item.text

        if not result_text:
            result_text = str(response)

        return result_text.strip(), marked

    def query_async(self, frame_bgr, fingertip_norm=None):
        """异步查询：在后台线程中调用 API"""
        with self.lock:
            if self.is_processing:
                return False
            self.is_processing = True

        thread = threading.Thread(
            target=self._query_async_worker,
            args=(frame_bgr.copy(), fingertip_norm),
            daemon=True
        )
        thread.start()
        return True

    def _query_async_worker(self, frame_bgr, fingertip_norm):
        try:
            answer, _ = self.query_sync(frame_bgr, fingertip_norm)
            with self.lock:
                self.last_answer = answer
                self.last_timestamp = time.time()
            print(f"[VLM-QA] 完成: {answer[:80]}")
        except Exception as e:
            with self.lock:
                self.last_answer = f"查询失败: {str(e)[:80]}"
                self.last_timestamp = time.time()
            print(f"[VLM-QA] 失败: {e}")
        finally:
            with self.lock:
                self.is_processing = False

    def get_state(self):
        with self.lock:
            return {
                "last_answer": self.last_answer,
                "last_timestamp": self.last_timestamp,
                "is_processing": self.is_processing,
            }


engine = VisualQAEngine()


def visualize_result(frame_bgr, state, fingertip_norm=None):
    """在画面上绘制 VLM 问答结果"""
    display = frame_bgr.copy()
    h, w = display.shape[:2]

    # 画指尖标记
    if fingertip_norm is not None:
        tip_px = (int(fingertip_norm[0] * w), int(fingertip_norm[1] * h))
        cv2.circle(display, tip_px, MARKER_RADIUS, (0, 0, 255), 3)
        cv2.circle(display, tip_px, MARKER_RADIUS + 5, (0, 0, 255), 2)

    # 顶部状态条
    overlay = display.copy()
    cv2.rectangle(overlay, (0, 0), (w, 110), (0, 0, 0), -1)
    display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

    answer = state.get("last_answer", "")
    is_processing = state.get("is_processing", False)

    if is_processing:
        display = put_chinese_text(display, "VLM 查询中...", (10, 10),
                                   engine.font_medium, (0, 255, 255))
    elif answer:
        # 多行显示（每行最多 30 个字符）
        lines = [answer[i:i+30] for i in range(0, len(answer), 30)]
        for i, line in enumerate(lines[:3]):
            display = put_chinese_text(display, line, (10, 10 + i * 28),
                                       engine.font_medium, (255, 255, 255))
        if len(lines) > 3:
            display = put_chinese_text(display, "...", (10, 10 + 3 * 28),
                                       engine.font_small, (180, 180, 180))

    # 底部状态条
    overlay2 = display.copy()
    cv2.rectangle(overlay2, (0, h - 28), (w, h), (0, 0, 0), -1)
    display = cv2.addWeighted(overlay2, 0.6, display, 0.4, 0)
    status = "VLM-QA | 查询中..." if is_processing else "VLM-QA | 空闲"
    display = put_chinese_text(display, status, (10, h - 22),
                               engine.font_small, (200, 200, 200))
    return display


@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "success": True,
        "service": "vlm-visual-qa",
        "model": MODEL_ENDPOINT,
        "processing": engine.is_processing,
    })


@app.route('/detect', methods=['POST'])
def detect():
    """同步调用 VLM：直接返回结果（适合手动触发）"""
    try:
        data = request.get_json()
        image_b64 = data.get('image')
        if not image_b64:
            return jsonify({"success": False, "error": "No image provided"}), 400

        # 解码图像
        frame = decode_image_b64(image_b64)
        if frame is None:
            return jsonify({"success": False, "error": "Invalid image"}), 400

        # 可选指尖归一化坐标
        fingertip_norm = None
        if 'fingertip_x' in data and 'fingertip_y' in data:
            try:
                fx = float(data['fingertip_x'])
                fy = float(data['fingertip_y'])
                if 0.0 <= fx <= 1.0 and 0.0 <= fy <= 1.0:
                    fingertip_norm = (fx, fy)
            except (ValueError, TypeError):
                pass

        t0 = time.time()
        answer, _ = engine.query_sync(frame, fingertip_norm)
        elapsed = time.time() - t0

        return jsonify({
            "success": True,
            "result": {
                "answer": answer,
                "timestamp": int(time.time() * 1000),
                "fingertip_provided": fingertip_norm is not None,
            },
            "processing_time": elapsed,
        })
    except Exception as e:
        print(f"[ERROR] /detect: {e}")
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    """同步调用 VLM：返回结果 + 可视化图像"""
    try:
        data = request.get_json()
        image_b64 = data.get('image')
        if not image_b64:
            return jsonify({"success": False, "error": "No image provided"}), 400

        frame = decode_image_b64(image_b64)
        if frame is None:
            return jsonify({"success": False, "error": "Invalid image"}), 400

        fingertip_norm = None
        if 'fingertip_x' in data and 'fingertip_y' in data:
            try:
                fx = float(data['fingertip_x'])
                fy = float(data['fingertip_y'])
                if 0.0 <= fx <= 1.0 and 0.0 <= fy <= 1.0:
                    fingertip_norm = (fx, fy)
            except (ValueError, TypeError):
                pass

        t0 = time.time()
        answer, _ = engine.query_sync(frame, fingertip_norm)
        elapsed = time.time() - t0

        # 生成可视化
        state = {
            "last_answer": answer,
            "is_processing": False,
        }
        vis = visualize_result(frame, state, fingertip_norm)
        vis_b64 = encode_b64(vis, 80)

        return jsonify({
            "success": True,
            "result": {
                "answer": answer,
                "timestamp": int(time.time() * 1000),
                "fingertip_provided": fingertip_norm is not None,
            },
            "visualized_image": vis_b64,
            "processing_time": elapsed,
        })
    except Exception as e:
        print(f"[ERROR] /detect_visualize: {e}")
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/trigger_async', methods=['POST'])
def trigger_async():
    """异步触发：在后台线程调用 API，立即返回。结果通过 /state 查询"""
    try:
        data = request.get_json()
        image_b64 = data.get('image')
        if not image_b64:
            return jsonify({"success": False, "error": "No image provided"}), 400

        frame = decode_image_b64(image_b64)
        if frame is None:
            return jsonify({"success": False, "error": "Invalid image"}), 400

        fingertip_norm = None
        if 'fingertip_x' in data and 'fingertip_y' in data:
            try:
                fx = float(data['fingertip_x'])
                fy = float(data['fingertip_y'])
                if 0.0 <= fx <= 1.0 and 0.0 <= fy <= 1.0:
                    fingertip_norm = (fx, fy)
            except (ValueError, TypeError):
                pass

        triggered = engine.query_async(frame, fingertip_norm)
        return jsonify({
            "success": True,
            "triggered": triggered,
            "is_processing": engine.is_processing,
        })
    except Exception as e:
        print(f"[ERROR] /trigger_async: {e}")
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/state', methods=['GET'])
def state():
    """查询当前 VLM 引擎状态"""
    s = engine.get_state()
    return jsonify({
        "success": True,
        "result": s,
    })


@app.route('/reset', methods=['POST'])
def reset():
    with engine.lock:
        engine.last_answer = ""
        engine.last_timestamp = 0
    return jsonify({"success": True})


if __name__ == '__main__':
    print(f"[INFO] VLM Visual QA Service starting on port {PORT}")
    print(f"[INFO] Model: {MODEL_ENDPOINT}")
    print(f"[INFO] Base URL: {BASE_URL}")
    print(f"[INFO] API Key configured: {'YES' if API_KEY else 'NO'}")
    app.run(host='0.0.0.0', port=PORT, threaded=True)
