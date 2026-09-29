"""
手机检测 - MediaPipe Object Detector + 豆包API验证

触发逻辑：
1. MediaPipe Object Detector 检测画面中的物体（COCO 80类）
2. 筛选 "cell phone" 类别（COCO class 67）
3. 检测到手机 + 置信度 > 阈值 → 发送图片给豆包API验证是否在玩游戏
4. 5秒冷却防止重复触发

操作：
- 按 'q' 退出
- 按 's' 手动触发API调用
- 按 'd' 切换调试信息（显示所有检测结果）
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

# 目标检测参数
PHONE_CONFIDENCE_THRESHOLD = 0.4  # 手机检测置信度阈值
TRIGGER_COOLDOWN = 5.0            # 触发冷却时间（秒）

# 模型路径
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "checkpoint", "ssd_mobilenet_v2.tflite")
# ==============================

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


class PhoneDetector:
    """MediaPipe 物体检测器：检测手机（COCO class: cell phone）"""

    # COCO 类别中 "cell phone" 的索引
    PHONE_CLASS_NAME = "cell phone"

    def __init__(self, model_path: str):
        self.detection_result = None

        def save_result(result: vision.ObjectDetectorResult, unused_output_image: mp.Image, timestamp_ms: int):
            self.detection_result = result

        base_options = python.BaseOptions(model_asset_path=model_path)
        options = vision.ObjectDetectorOptions(
            base_options=base_options,
            running_mode=vision.RunningMode.LIVE_STREAM,
            score_threshold=0.3,       # 较低阈值，后续手动筛选
            max_results=10,
            result_callback=save_result,
        )
        self.detector = vision.ObjectDetector.create_from_options(options)

    def run(self, image: np.ndarray):
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image)
        self.detector.detect_async(mp_image, time.time_ns() // 1_000_000)

    def get_result(self) -> dict:
        """获取检测结果，返回所有检测和手机检测结果"""
        all_detections = []
        phone_detections = []

        if self.detection_result and self.detection_result.detections:
            for detection in self.detection_result.detections:
                bbox = detection.bounding_box
                categories = detection.categories

                if categories:
                    cat = categories[0]
                    det_info = {
                        'category_name': cat.category_name,
                        'score': cat.score,
                        'bbox': (bbox.origin_x, bbox.origin_y, bbox.width, bbox.height),
                    }
                    all_detections.append(det_info)

                    # 筛选手机
                    if cat.category_name == self.PHONE_CLASS_NAME and cat.score >= PHONE_CONFIDENCE_THRESHOLD:
                        phone_detections.append(det_info)

        return {
            'all_detections': all_detections,
            'phone_detections': phone_detections,
        }

    def close(self):
        self.detector.close()


class DoubaoVerifier:
    """豆包API验证器：判断是否在玩游戏"""

    PROMPT = """请观察这张图片中的人，判断他/她是否在玩手机游戏或电脑游戏。

判断依据：
- 正在玩游戏：手机屏幕显示游戏画面、手持手机玩游戏、电脑屏幕显示游戏界面
- 不是玩游戏：刷短视频、看小说、打电话、正常办公等

请严格按以下格式回答：
判断：是/否/不确定
理由：一句话说明"""

    def __init__(self):
        self.client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
        self.last_result = ""
        self.last_reason = ""
        self.is_processing = False
        self.lock = threading.Lock()

    def verify(self, frame):
        if self.is_processing:
            return
        self.is_processing = True
        thread = threading.Thread(target=self._verify_sync, args=(frame,), daemon=True)
        thread.start()

    def _verify_sync(self, frame):
        try:
            h, w = frame.shape[:2]
            if w > MAX_IMAGE_WIDTH:
                scale = MAX_IMAGE_WIDTH / w
                frame = cv2.resize(frame, (MAX_IMAGE_WIDTH, int(h * scale)))

            _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            b64_image = base64.b64encode(buffer).decode('utf-8')
            data_uri = f"data:image/jpeg;base64,{b64_image}"

            response = self.client.responses.create(
                model=MODEL_ENDPOINT,
                input=[{
                    "role": "user",
                    "content": [
                        {"type": "input_image", "image_url": data_uri},
                        {"type": "input_text", "text": self.PROMPT},
                    ],
                }],
                extra_body={
                    "thinking": {"type": "disabled"},  # 关闭深度思考模式，降低响应时间
                },
            )

            result_text = ""
            if hasattr(response, 'output') and response.output:
                for item in response.output:
                    if hasattr(item, 'content') and item.content:
                        for content_item in item.content:
                            if hasattr(content_item, 'text'):
                                result_text += content_item.text

            judgment = "不确定"
            reason = ""
            for line in result_text.split('\n'):
                line = line.strip()
                if line.startswith('判断') and ('：' in line or ':' in line):
                    judgment = line.split('：', 1)[-1].split(':', 1)[-1].strip()
                elif line.startswith('理由') and ('：' in line or ':' in line):
                    reason = line.split('：', 1)[-1].split(':', 1)[-1].strip()

            with self.lock:
                self.last_result = judgment
                self.last_reason = reason

            print(f"[API验证完成] 判断: {judgment} | 理由: {reason}")

        except Exception as e:
            with self.lock:
                self.last_result = "失败"
                self.last_reason = str(e)[:50]
            print(f"[API验证失败] {e}")
        finally:
            self.is_processing = False

    def get_result(self):
        with self.lock:
            return self.last_result, self.last_reason


def main():
    print("=" * 60)
    print("手机检测 - MediaPipe Object Detector + 豆包API验证")
    print("=" * 60)
    print(f"模型: {MODEL_PATH}")
    print(f"手机置信度阈值: {PHONE_CONFIDENCE_THRESHOLD}")
    print(f"触发冷却: {TRIGGER_COOLDOWN}秒")
    print("按 'q' 退出 | 's' 手动触发API | 'd' 切换调试显示")
    print("=" * 60)

    if not os.path.exists(MODEL_PATH):
        print(f"错误：模型文件不存在: {MODEL_PATH}")
        return

    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"错误：无法打开摄像头 (index={CAMERA_INDEX})")
        return

    detector = PhoneDetector(model_path=MODEL_PATH)
    verifier = DoubaoVerifier()

    last_trigger_time = 0
    show_debug = True

    font_medium = _find_chinese_font(18)
    font_small = _find_chinese_font(14)

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            frame = cv2.flip(frame, 1)
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

            # 物体检测
            detector.run(image=rgb_frame)
            result = detector.get_result()

            all_dets = result['all_detections']
            phone_dets = result['phone_detections']

            # 是否检测到手机
            phone_detected = len(phone_dets) > 0

            # 触发逻辑
            current_time = time.time()
            should_trigger = (
                phone_detected and
                current_time - last_trigger_time > TRIGGER_COOLDOWN and
                not verifier.is_processing
            )

            if should_trigger:
                last_trigger_time = current_time
                verifier.verify(frame.copy())
                print(f"[触发] {time.strftime('%H:%M:%S')} 检测到手机，发送API验证...")

            # 获取API结果
            api_result, api_reason = verifier.get_result()

            # 显示
            display = frame.copy()

            # 绘制所有检测结果（调试模式）
            if show_debug:
                for det in all_dets:
                    x, y, w, h = det['bbox']
                    name = det['category_name']
                    score = det['score']

                    if name == "cell phone":
                        color = (0, 0, 255)  # 红色：手机
                        cv2.rectangle(display, (x, y), (x + w, y + h), color, 2)
                        label = f"{name} {score:.2f}"
                        cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), color, -1)
                        cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
                    else:
                        color = (100, 100, 100)  # 灰色：其他物体
                        cv2.rectangle(display, (x, y), (x + w, y + h), color, 1)

            # 顶部状态栏
            overlay = display.copy()
            cv2.rectangle(overlay, (0, 0), (display.shape[1], 100), (0, 0, 0), -1)
            display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

            # 手机检测状态
            phone_color = (0, 255, 0) if phone_detected else (100, 100, 100)
            phone_text = f"手机: {'检测到' if phone_detected else '未检测到'}"
            if phone_dets:
                phone_text += f" ({phone_dets[0]['score']:.2f})"
            display = put_chinese_text(display, phone_text, (10, 8), font_small, phone_color)

            # API状态
            api_status = f"API: {'验证中' if verifier.is_processing else '空闲'}"
            if verifier.is_processing:
                cooldown = max(0, TRIGGER_COOLDOWN - (current_time - last_trigger_time))
                api_status += f" | 冷却: {cooldown:.0f}s"
            display = put_chinese_text(display, api_status, (10, 30), font_small, (255, 255, 255))

            # API结果
            if api_result:
                color_map = {"是": (0, 0, 255), "否": (0, 255, 0), "不确定": (0, 255, 255)}
                result_color = color_map.get(api_result, (200, 200, 200))
                display = put_chinese_text(display, f"结果: {api_result}", (10, 52), font_medium, result_color)
                if api_reason:
                    display = put_chinese_text(display, f"理由: {api_reason[:25]}", (10, 78), font_small, (255, 255, 255))

            cv2.imshow("Phone Detector", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('s'):
                verifier.verify(frame.copy())
                last_trigger_time = current_time
                print(f"[手动触发] {time.strftime('%H:%M:%S')}")
            elif key == ord('d'):
                show_debug = not show_debug

    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        detector.close()
        print("资源已释放")


if __name__ == "__main__":
    main()
