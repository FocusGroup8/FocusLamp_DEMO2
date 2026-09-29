"""
豆包视觉大模型 - 游戏检测独立测试脚本

功能：
1. 每10秒从摄像头采集一帧
2. 将帧编码为 Base64 发送给豆包 API
3. 解析模型返回的"是否在玩游戏"判断结果
4. 在画面上显示检测结果

使用前：
1. pip install --upgrade "openai>=1.0"
2. 确保摄像头可用
3. 设置环境变量 ARK_API_KEY 或直接修改下方 API_KEY
"""

import os
import sys
import time
import base64
import threading
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from openai import OpenAI

# ============ 配置 ============
# 密钥必须通过环境变量 ARK_API_KEY 注入，禁止硬编码
API_KEY = os.getenv("ARK_API_KEY", "")
MODEL_ENDPOINT = "doubao-seed-2-0-mini-260428"
BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
CAMERA_INDEX = 0
SAMPLE_INTERVAL = 10  # 秒
JPEG_QUALITY = 50     # 压缩质量（0-100），越低越省 token
MAX_IMAGE_WIDTH = 640 # 最大宽度，超过则缩放
# ==============================

# 中文字体路径（Windows 常见字体）
FONT_CANDIDATES = [
    "C:/Windows/Fonts/msyh.ttc",      # 微软雅黑
    "C:/Windows/Fonts/simhei.ttf",     # 黑体
    "C:/Windows/Fonts/simsun.ttc",     # 宋体
]


def _find_chinese_font(size=20):
    """查找可用的中文字体"""
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def put_chinese_text(img, text, pos, font, color=(255, 255, 255)):
    """在 OpenCV 图像上绘制中文文本"""
    pil_img = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(pil_img)
    draw.text(pos, text, font=font, fill=color[::-1])  # BGR -> RGB
    return cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)

SYSTEM_PROMPT = """你是一个行为分析助手。请观察这张图片中的人，判断他/她是否在玩游戏（包括电脑游戏和手机游戏）。

判断依据：
- 正在玩游戏：屏幕显示游戏画面、手持游戏手柄、键盘快速操作、明显的游戏界面、手持手机玩游戏（横屏或竖屏游戏画面）、低头专注手机屏幕且手指快速滑动
- 不是玩游戏：正常办公、浏览网页、看视频、写代码、刷社交媒体、打电话等日常使用

请严格按以下格式回答，不要输出其他内容：
判断：是/否/不确定
理由：一句话说明"""


class GameDetector:
    """游戏检测器：调用豆包视觉API判断用户是否在玩游戏"""

    def __init__(self):
        self.client = OpenAI(
            base_url=BASE_URL,
            api_key=API_KEY,
        )
        self.last_result = "等待检测..."
        self.last_reason = ""
        self.last_time = 0
        self.is_processing = False
        self.lock = threading.Lock()

    def detect(self, frame):
        """异步检测：在后台线程中调用API"""
        if self.is_processing:
            return
        self.is_processing = True
        thread = threading.Thread(target=self._detect_sync, args=(frame,), daemon=True)
        thread.start()

    def _detect_sync(self, frame):
        """同步调用API（在后台线程中执行）"""
        try:
            # 1. 图像预处理：缩放 + 压缩
            h, w = frame.shape[:2]
            if w > MAX_IMAGE_WIDTH:
                scale = MAX_IMAGE_WIDTH / w
                frame = cv2.resize(frame, (MAX_IMAGE_WIDTH, int(h * scale)))

            # 2. 编码为 Base64
            _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            b64_image = base64.b64encode(buffer).decode('utf-8')
            data_uri = f"data:image/jpeg;base64,{b64_image}"

            # 3. 调用豆包 Responses API
            response = self.client.responses.create(
                model=MODEL_ENDPOINT,
                input=[
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "input_image",
                                "image_url": data_uri,
                            },
                            {
                                "type": "input_text",
                                "text": SYSTEM_PROMPT,
                            },
                        ],
                    }
                ],
                extra_body={
                    "thinking": {"type": "disabled"},  # 关闭深度思考模式，降低响应时间
                },
            )

            # 4. 解析结果
            result_text = ""
            # Responses API 返回结构：output 是一个列表
            if hasattr(response, 'output') and response.output:
                for item in response.output:
                    if hasattr(item, 'content') and item.content:
                        for content_item in item.content:
                            if hasattr(content_item, 'text'):
                                result_text += content_item.text

            if not result_text:
                # 尝试其他解析方式
                result_text = str(response)

            # 5. 提取判断和理由
            judgment = "不确定"
            reason = ""
            for line in result_text.split('\n'):
                line = line.strip()
                if line.startswith('判断：') or line.startswith('判断:'):
                    judgment = line.split('：', 1)[-1].split(':', 1)[-1].strip()
                elif line.startswith('理由：') or line.startswith('理由:'):
                    reason = line.split('：', 1)[-1].split(':', 1)[-1].strip()

            with self.lock:
                self.last_result = judgment
                self.last_reason = reason
                self.last_time = time.time()

            print(f"[检测完成] 判断: {judgment} | 理由: {reason}")

        except Exception as e:
            with self.lock:
                self.last_result = "检测失败"
                self.last_reason = str(e)[:50]
            print(f"[检测失败] {e}")
        finally:
            self.is_processing = False

    def get_result(self):
        """获取最近一次检测结果"""
        with self.lock:
            return self.last_result, self.last_reason, self.last_time


def main():
    print("=" * 50)
    print("豆包视觉大模型 - 游戏检测测试")
    print(f"模型端点: {MODEL_ENDPOINT}")
    print(f"采样间隔: {SAMPLE_INTERVAL}秒")
    print(f"图片质量: JPEG Q={JPEG_QUALITY}, 最大宽度={MAX_IMAGE_WIDTH}")
    print("按 'q' 退出, 按 's' 立即采样")
    print("=" * 50)

    # 初始化摄像头
    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"错误：无法打开摄像头 (index={CAMERA_INDEX})")
        return

    # 初始化检测器
    detector = GameDetector()
    last_sample_time = 0

    # 加载中文字体
    font_large = _find_chinese_font(24)
    font_medium = _find_chinese_font(18)
    font_small = _find_chinese_font(14)

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("错误：无法读取摄像头画面")
                break

            frame = cv2.flip(frame, 1)  # 镜像翻转
            current_time = time.time()

            # 定时采样
            if current_time - last_sample_time >= SAMPLE_INTERVAL:
                last_sample_time = current_time
                detector.detect(frame.copy())
                print(f"[采样] {time.strftime('%H:%M:%S')} 发送检测请求...")

            # 获取检测结果
            result, reason, detect_time = detector.get_result()

            # 在画面上显示结果
            display = frame.copy()

            # 检测结果颜色
            color_map = {"是": (0, 0, 255), "否": (0, 255, 0), "不确定": (0, 255, 255)}
            result_color = color_map.get(result, (200, 200, 200))

            display = put_chinese_text(display, f"游戏检测: {result}", (10, 10), font_large, result_color)
            if reason:
                display = put_chinese_text(display, f"理由: {reason[:30]}", (10, 40), font_medium, result_color)

            # 状态信息
            status = "检测中..." if detector.is_processing else "空闲"
            next_sample = max(0, SAMPLE_INTERVAL - (current_time - last_sample_time))
            display = put_chinese_text(display, f"状态: {status} | 下次采样: {next_sample:.0f}s", (10, 65), font_small, (200, 200, 200))

            cv2.imshow("Doubao Game Detector", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('s'):
                # 手动触发采样
                last_sample_time = current_time
                detector.detect(frame.copy())
                print(f"[手动采样] {time.strftime('%H:%M:%S')} 发送检测请求...")

    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        print("资源已释放")


if __name__ == "__main__":
    main()
