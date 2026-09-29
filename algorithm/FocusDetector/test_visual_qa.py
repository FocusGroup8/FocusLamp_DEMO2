"""
视觉问答 MVP 测试脚本

功能：
1. 检测食指指向手势（Pointing_Up）持续 1.5 秒 → 触发问答
2. 在指尖位置画红色圆圈标记
3. 发送标记后的图片 + Prompt 给豆包 API
4. 在画面上显示模型回复

交互：
- 用食指指向目标，保持 1.5 秒 → 自动触发描述
- 按 'q' 退出
- 按 's' 手动触发（用当前指尖位置，无手势要求）
"""

import os
import time
import base64
import threading
import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
from PIL import Image, ImageDraw, ImageFont
from openai import OpenAI

# ============ 配置 ============
# 密钥必须通过环境变量 ARK_API_KEY 注入，禁止硬编码
API_KEY = os.getenv("ARK_API_KEY", "")
MODEL_ENDPOINT = "doubao-seed-2-0-mini-260428"
BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
CAMERA_INDEX = 0
JPEG_QUALITY = 50
MAX_IMAGE_WIDTH = 640
POINTING_HOLD_THRESHOLD = 1.5  # 指向手势持续触发阈值（秒）
MARKER_RADIUS = 20             # 标记圆圈半径（像素）
# ==============================

# 中文字体
FONT_CANDIDATES = [
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
]


def _find_chinese_font(size=20):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def put_chinese_text(img, text, pos, font, color=(255, 255, 255)):
    pil_img = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(pil_img)
    draw.text(pos, text, font=font, fill=color[::-1])
    return cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)


# 视觉问答 Prompt
VA_PROMPT = """用户正在用食指指向画面中的某个位置，该位置已被红色圆圈标记。
请仔细观察红圈标记处的内容，用简洁的中文描述该位置是什么。

描述要求：
- 如果是文字，请读出文字内容
- 如果是物体，请描述物体名称和特征
- 如果是人，请描述其外观特征
- 如果是屏幕上的内容，请描述具体内容

请直接描述，不要加前缀："""


class GestureDetector:
    """MediaPipe 手势检测器（提取手势+手部关键点）"""

    def __init__(self, model_path="checkpoint/gesture_recognizer.task"):
        self.gesture_category = None
        self.gesture_score = 0.0
        self.hand_landmarks = []
        self._result = None

        def save_result(result, unused_output_image, timestamp_ms):
            try:
                self.gesture_category = None
                self.gesture_score = 0.0
                self.hand_landmarks = []
                self._result = result

                if result.gestures and result.gestures[0]:
                    gesture = result.gestures[0][0]
                    self.gesture_category = gesture.category_name
                    self.gesture_score = gesture.score

                if result.hand_landmarks:
                    for hand in result.hand_landmarks:
                        pts = [(lm.x, lm.y) for lm in hand]
                        self.hand_landmarks.append(pts)
            except Exception as e:
                print(f"[手势检测错误] {e}")

        base_options = python.BaseOptions(model_asset_path=model_path)
        options = vision.GestureRecognizerOptions(
            base_options=base_options,
            running_mode=vision.RunningMode.LIVE_STREAM,
            num_hands=1,
            min_hand_detection_confidence=0.5,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
            result_callback=save_result,
        )
        self.recognizer = vision.GestureRecognizer.create_from_options(options)

    def run(self, image: np.ndarray):
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
        self.recognizer.recognize_async(mp_image, time.time_ns() // 1_000_000)

    def get_result(self):
        return {
            'gesture': self.gesture_category,
            'hand_landmarks': self.hand_landmarks,
        }

    def close(self):
        self.recognizer.close()


class VisualQADetector:
    """视觉问答检测器：指向触发 + 豆包API描述"""

    def __init__(self):
        self.client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
        self.last_answer = ""
        self.is_processing = False
        self.lock = threading.Lock()

    def query(self, frame):
        """异步查询：在后台线程中调用API"""
        if self.is_processing:
            return
        self.is_processing = True
        thread = threading.Thread(target=self._query_sync, args=(frame,), daemon=True)
        thread.start()

    def _query_sync(self, frame):
        try:
            # 缩放
            h, w = frame.shape[:2]
            if w > MAX_IMAGE_WIDTH:
                scale = MAX_IMAGE_WIDTH / w
                frame = cv2.resize(frame, (MAX_IMAGE_WIDTH, int(h * scale)))

            # Base64 编码
            _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
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
                    "thinking": {"type": "disabled"},  # 关闭深度思考模式，降低响应时间
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

            with self.lock:
                self.last_answer = result_text.strip()

            print(f"[视觉问答完成] {self.last_answer[:80]}")

        except Exception as e:
            with self.lock:
                self.last_answer = f"查询失败: {str(e)[:50]}"
            print(f"[视觉问答失败] {e}")
        finally:
            self.is_processing = False

    def get_result(self):
        with self.lock:
            return self.last_answer


def main():
    print("=" * 50)
    print("视觉问答 MVP 测试")
    print("用食指指向目标，保持1.5秒 → 自动触发描述")
    print("按 'q' 退出, 按 's' 手动触发")
    print("=" * 50)

    # 初始化摄像头
    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"错误：无法打开摄像头 (index={CAMERA_INDEX})")
        return

    # 初始化手势检测器
    gesture_detector = GestureDetector(
        model_path=os.path.join(os.path.dirname(__file__), "checkpoint", "gesture_recognizer.task")
    )

    # 初始化视觉问答
    qa_detector = VisualQADetector()

    # 指向状态跟踪
    pointing_start_time = None
    is_triggered = False  # 防止重复触发

    # 字体
    font_large = _find_chinese_font(22)
    font_medium = _find_chinese_font(16)
    font_small = _find_chinese_font(13)

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            frame = cv2.flip(frame, 1)
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

            # 手势检测
            gesture_detector.run(image=rgb_frame)
            gesture_result = gesture_detector.get_result()

            gesture = gesture_result['gesture']
            hand_landmarks = gesture_result['hand_landmarks']

            # 获取食指尖位置（landmark 8）
            fingertip = None
            if hand_landmarks:
                hand = hand_landmarks[0]
                if len(hand) > 8:
                    fingertip = hand[8]  # (x, y) 归一化坐标

            # 指向手势持续检测
            current_time = time.time()
            is_pointing = (gesture == "Pointing_Up")

            if is_pointing and fingertip:
                if pointing_start_time is None:
                    pointing_start_time = current_time
                    is_triggered = False
                elif not is_triggered and (current_time - pointing_start_time >= POINTING_HOLD_THRESHOLD):
                    # 触发视觉问答
                    is_triggered = True
                    h, w = frame.shape[:2]
                    tip_px = (int(fingertip[0] * w), int(fingertip[1] * h))

                    # 在画面上画红色圆圈标记
                    marked_frame = frame.copy()
                    cv2.circle(marked_frame, tip_px, MARKER_RADIUS, (0, 0, 255), 3)
                    cv2.circle(marked_frame, tip_px, MARKER_RADIUS + 5, (0, 0, 255), 2)

                    qa_detector.query(marked_frame)
                    print(f"[触发] 指尖位置: {tip_px}, 发送视觉问答请求...")
            else:
                pointing_start_time = None
                is_triggered = False

            # 显示画面
            display = frame.copy()

            # 绘制指尖标记和指向进度
            if fingertip:
                h, w = frame.shape[:2]
                tip_px = (int(fingertip[0] * w), int(fingertip[1] * h))

                if is_pointing:
                    # 指向进度条
                    if pointing_start_time:
                        progress = min(1.0, (current_time - pointing_start_time) / POINTING_HOLD_THRESHOLD)
                        # 画进度环
                        cv2.ellipse(display, tip_px, (MARKER_RADIUS + 8, MARKER_RADIUS + 8),
                                    0, -90, -90 + 360 * progress, (0, 255, 0), 3)
                        # 画指尖点
                        cv2.circle(display, tip_px, 6, (0, 255, 0), -1)
                    else:
                        cv2.circle(display, tip_px, 6, (0, 255, 0), -1)
                else:
                    cv2.circle(display, tip_px, 4, (200, 200, 200), -1)

            # 显示问答结果
            answer = qa_detector.get_result()
            if answer:
                # 半透明黑色背景条
                overlay = display.copy()
                cv2.rectangle(overlay, (0, 0), (display.shape[1], 120), (0, 0, 0), -1)
                display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

                # 多行显示（每行最多20个字符），白色文字
                lines = []
                for i in range(0, len(answer), 20):
                    lines.append(answer[i:i+20])
                for i, line in enumerate(lines[:4]):  # 最多显示4行
                    display = put_chinese_text(display, line, (10, 8 + i * 26), font_medium, (255, 255, 255))
                if len(lines) > 4:
                    display = put_chinese_text(display, "...", (10, 8 + 4 * 26), font_small, (180, 180, 180))

            # 状态信息（底部，白色文字+半透明背景）
            status_parts = []
            if gesture:
                status_parts.append(f"手势: {gesture}")
            if qa_detector.is_processing:
                status_parts.append("查询中...")
            status_text = " | ".join(status_parts) if status_parts else "空闲"
            overlay2 = display.copy()
            cv2.rectangle(overlay2, (0, display.shape[0] - 30), (display.shape[1], display.shape[0]), (0, 0, 0), -1)
            display = cv2.addWeighted(overlay2, 0.6, display, 0.4, 0)
            display = put_chinese_text(display, status_text, (10, display.shape[0] - 25), font_small, (255, 255, 255))

            cv2.imshow("Visual QA", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('s'):
                # 手动触发
                if fingertip:
                    h, w = frame.shape[:2]
                    tip_px = (int(fingertip[0] * w), int(fingertip[1] * h))
                    marked_frame = frame.copy()
                    cv2.circle(marked_frame, tip_px, MARKER_RADIUS, (0, 0, 255), 3)
                    cv2.circle(marked_frame, tip_px, MARKER_RADIUS + 5, (0, 0, 255), 2)
                    qa_detector.query(marked_frame)
                    print(f"[手动触发] 指尖位置: {tip_px}")
                else:
                    print("[手动触发] 未检测到手部，无法定位指尖")

    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        gesture_detector.close()
        print("资源已释放")


if __name__ == "__main__":
    main()
