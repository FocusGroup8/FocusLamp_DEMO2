"""
专注检测 - 手机/电脑场景 + 豆包API验证

触发逻辑：
1. YOLOv8 检测画面中的 phone / mouse / keyboard（COCO 类别）
2. MediaPipe 检测手部关键点
3. 判断手部关键点是否在目标 bbox 内或边缘
   - 手机场景：手部接触手机
   - 电脑场景：任一手接触鼠标或键盘
4. 任一场景满足 → 发送图片给豆包API验证是否在玩游戏
5. 冷却防止重复触发

操作：
- 按 'q' 退出
- 按 's' 手动触发API调用
- 按 'd' 切换调试信息
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
from ultralytics import YOLO
from PIL import Image, ImageDraw, ImageFont
from openai import OpenAI

# ============ 配置 ============
# 密钥必须通过环境变量 ARK_API_KEY 注入，禁止硬编码
API_KEY = os.getenv("ARK_API_KEY", "")
MODEL_ENDPOINT = "ep-20260615144034-b5zxv"  # 豆包 2.0 pro 端点（对比测试用）
BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
CAMERA_INDEX = 1  # 外接 USB 摄像头（0=内置, 1=USB）
JPEG_QUALITY = 50                      # JPEG 压缩质量
MAX_IMAGE_WIDTH = 640                   # 最大图片宽度

# YOLOv8 参数
YOLO_MODEL = "yolo26s.pt"            # YOLO26s（2026最新,优化小物体检测,NMS-Free）
PHONE_CLASS_ID = 67                   # COCO 0-indexed: cell phone
MOUSE_CLASS_ID = 64                  # COCO 0-indexed: mouse（修正:73 是 book）
KEYBOARD_CLASS_ID = 66               # COCO 0-indexed: keyboard（修正:76 是 scissors）
PHONE_CONFIDENCE_THRESHOLD = 0.4     # 手机检测置信度阈值
PC_CONFIDENCE_THRESHOLD = 0.25        # 鼠标/键盘检测置信度阈值（降低，YOLOv8n对俯视角度检测较弱）
TRIGGER_COOLDOWN = 5.0               # 触发冷却时间（秒）
INFERENCE_INTERVAL = 5               # 推理间隔帧数（每 N 帧推理一次，API调用期间自动暂停）

# MediaPipe 手部参数
HAND_LANDMARKER_PATH = os.path.join(os.path.dirname(__file__), "checkpoint", "hand_landmarker.task")
HAND_CONTACT_MARGIN = 25             # 手部关键点与手机bbox边缘的容差（像素）
# 用于判断"接触"的关键landmark索引：手腕、食指根、中指根、小指根
HAND_KEY_LANDMARKS = [0, 5, 9, 13, 17]
HAND_MIN_DETECTION_CONFIDENCE = 0.3   # 首次检测阈值（降低以解决冷启动问题）
HAND_MIN_PRESENCE_CONFIDENCE = 0.3    # 手部存在判断阈值
HAND_MIN_TRACKING_CONFIDENCE = 0.5    # 跟踪阈值（保持稳定，避免频繁重检测）
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


def is_landmark_in_bbox(lm_x, lm_y, bbox, margin=0):
    """判断关键点是否在bbox内（含边缘容差）"""
    x, y, w, h = bbox
    return (x - margin) <= lm_x <= (x + w + margin) and (y - margin) <= lm_y <= (y + h + margin)


def check_hand_touching_phone(hand_landmarks_list, phone_bbox, img_w, img_h):
    """
    判断手部是否接触手机。
    hand_landmarks_list: MediaPipe tasks API 返回的 hand_landmarks（List[List[NormalizedLandmark]]）
    phone_bbox: (x, y, w, h)
    返回: (是否接触, 接触的手部关键点坐标列表)
    """
    if not hand_landmarks_list:
        return False, []

    contact_points = []
    for hand_lms in hand_landmarks_list:
        # tasks API: hand_lms 是 List[NormalizedLandmark]
        # 旧 API: hand_lms.landmark 是 List[NormalizedLandmark]
        landmarks = hand_lms.landmark if hasattr(hand_lms, 'landmark') else hand_lms
        for idx in HAND_KEY_LANDMARKS:
            if idx < len(landmarks):
                lm = landmarks[idx]
                px, py = int(lm.x * img_w), int(lm.y * img_h)
                if is_landmark_in_bbox(px, py, phone_bbox, HAND_CONTACT_MARGIN):
                    contact_points.append((px, py))

    return len(contact_points) > 0, contact_points


class HandDetector:
    """MediaPipe 手部关键点检测器（tasks API）"""

    # MediaPipe Hands 21个关键点连接关系
    HAND_CONNECTIONS = [
        (0, 1), (1, 2), (2, 3), (3, 4),          # 拇指
        (0, 5), (5, 6), (6, 7), (7, 8),          # 食指
        (5, 9), (9, 10), (10, 11), (11, 12),     # 中指
        (9, 13), (13, 14), (14, 15), (15, 16),    # 无名指
        (13, 17), (0, 17), (17, 18), (18, 19), (19, 20),  # 小指
    ]

    def __init__(self):
        self._last_landmarks = []  # List[List[NormalizedLandmark]]
        self._result_ready = False

        def save_result(result, unused_output_image, timestamp_ms):
            try:
                if result and result.hand_landmarks:
                    self._last_landmarks = result.hand_landmarks
                else:
                    self._last_landmarks = []
                self._result_ready = True
            except Exception:
                self._last_landmarks = []
                self._result_ready = True

        base_options = python.BaseOptions(model_asset_path=HAND_LANDMARKER_PATH)
        options = vision.HandLandmarkerOptions(
            base_options=base_options,
            running_mode=vision.RunningMode.LIVE_STREAM,
            num_hands=2,
            min_hand_detection_confidence=HAND_MIN_DETECTION_CONFIDENCE,
            min_hand_presence_confidence=HAND_MIN_PRESENCE_CONFIDENCE,
            min_tracking_confidence=HAND_MIN_TRACKING_CONFIDENCE,
            result_callback=save_result,
        )
        self.recognizer = vision.HandLandmarker.create_from_options(options)

    def detect(self, frame):
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=frame)
        self.recognizer.detect_async(mp_image, time.time_ns() // 1_000_000)

    def get_landmarks(self):
        return self._last_landmarks

    def draw(self, frame):
        """绘制手部骨架"""
        if not self._last_landmarks:
            return frame
        h, w = frame.shape[:2]
        for hand_lms in self._last_landmarks:
            # tasks API: hand_lms 是 List[NormalizedLandmark]
            landmarks = hand_lms.landmark if hasattr(hand_lms, 'landmark') else hand_lms
            pts = []
            for lm in landmarks:
                pts.append((int(lm.x * w), int(lm.y * h)))
            # 画连接线
            for a, b in self.HAND_CONNECTIONS:
                if a < len(pts) and b < len(pts):
                    cv2.line(frame, pts[a], pts[b], (255, 0, 0), 2)
            # 画关键点
            for px, py in pts:
                cv2.circle(frame, (px, py), 3, (0, 0, 255), -1)
        return frame

    def close(self):
        pass


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
        t_start = time.time()
        try:
            h, w = frame.shape[:2]
            if w > MAX_IMAGE_WIDTH:
                scale = MAX_IMAGE_WIDTH / w
                frame = cv2.resize(frame, (MAX_IMAGE_WIDTH, int(h * scale)))

            _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            b64_image = base64.b64encode(buffer).decode('utf-8')
            data_uri = f"data:image/jpeg;base64,{b64_image}"

            t_encode = time.time() - t_start
            t_api_start = time.time()
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
            t_api = time.time() - t_api_start
            t_parse_start = time.time()

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

            t_parse = time.time() - t_parse_start
            t_total = time.time() - t_start

            with self.lock:
                self.last_result = judgment
                self.last_reason = reason

            print(f"[API验证完成] 判断: {judgment} | 理由: {reason}")
            print(f"[耗时] 编码: {t_encode*1000:.0f}ms | API: {t_api*1000:.0f}ms | 解析: {t_parse*1000:.0f}ms | 总计: {t_total*1000:.0f}ms")

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
    print("专注检测 - 手机/电脑场景 + 豆包API验证")
    print("=" * 60)
    print(f"模型: {YOLO_MODEL}")
    print(f"检测类别: phone({PHONE_CLASS_ID}) mouse({MOUSE_CLASS_ID}) keyboard({KEYBOARD_CLASS_ID})")
    print(f"置信度阈值: 手机{PHONE_CONFIDENCE_THRESHOLD} / 电脑{PC_CONFIDENCE_THRESHOLD}")
    print(f"手部接触容差: {HAND_CONTACT_MARGIN}px")
    print(f"触发冷却: {TRIGGER_COOLDOWN}秒")
    print("按 'q' 退出 | 's' 手动触发API | 'd' 切换调试显示")
    print("=" * 60)

    # 初始化 YOLOv8（首次运行自动下载模型）
    print("正在加载 YOLOv8 模型...")
    model = YOLO(YOLO_MODEL)
    print("模型加载完成！")

    # 初始化 MediaPipe 手部检测器
    print("正在初始化手部检测器...")
    hand_detector = HandDetector()
    print("手部检测器就绪！")

    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"错误：无法打开摄像头 (index={CAMERA_INDEX})")
        return

    verifier = DoubaoVerifier()
    last_trigger_time = 0
    show_debug = True
    frame_count = 0
    # 检测结果缓存（避免非推理帧检测框闪烁）
    last_phone_detections = []
    last_pc_detections = []
    last_all_detections = []

    font_medium = _find_chinese_font(18)
    font_small = _find_chinese_font(14)

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            frame = cv2.flip(frame, 1)
            frame_count += 1
            h, w = frame.shape[:2]

            # 手部检测（每帧检测，MediaPipe 轻量级）
            hand_detector.detect(frame)
            hand_landmarks = hand_detector.get_landmarks()

            # 每 N 帧推理一次（降低 CPU 负载）
            # 使用缓存避免闪烁：非推理帧使用上次结果
            if frame_count % INFERENCE_INTERVAL == 0 and not verifier.is_processing:
                # YOLOv8 推理：同时检测 phone、mouse、keyboard
                # API 调用期间暂停 YOLO 推理，减少 CPU 竞争，加速 API 响应
                target_classes = [PHONE_CLASS_ID, MOUSE_CLASS_ID, KEYBOARD_CLASS_ID]
                results = model(frame, verbose=False, classes=target_classes)

                phone_detections = []
                pc_detections = []  # 鼠标+键盘检测
                all_detections = []

                for result in results:
                    boxes = result.boxes
                    for box in boxes:
                        cls_id = int(box.cls[0])
                        conf = float(box.conf[0])
                        x1, y1, x2, y2 = box.xyxy[0].tolist()
                        x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)

                        if cls_id == PHONE_CLASS_ID and conf >= PHONE_CONFIDENCE_THRESHOLD:
                            phone_detections.append({
                                'bbox': (x1, y1, x2 - x1, y2 - y1),
                                'score': conf,
                            })
                        elif cls_id in (MOUSE_CLASS_ID, KEYBOARD_CLASS_ID) and conf >= PC_CONFIDENCE_THRESHOLD:
                            pc_detections.append({
                                'bbox': (x1, y1, x2 - x1, y2 - y1),
                                'score': conf,
                                'cls_id': cls_id,
                                'name': 'mouse' if cls_id == MOUSE_CLASS_ID else 'keyboard',
                            })

                # 调试模式：获取所有检测结果（复用同一次推理,避免重复调用）
                if show_debug:
                    all_results = model(frame, verbose=False)
                    for result in all_results:
                        for box in result.boxes:
                            cls_id = int(box.cls[0])
                            conf = float(box.conf[0])
                            x1, y1, x2, y2 = box.xyxy[0].tolist()
                            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
                            class_name = model.names.get(cls_id, str(cls_id))
                            all_detections.append({
                                'bbox': (x1, y1, x2 - x1, y2 - y1),
                                'score': conf,
                                'name': class_name,
                            })

                # 缓存本次结果（供非推理帧使用,避免闪烁）
                last_phone_detections = phone_detections
                last_pc_detections = pc_detections
                last_all_detections = all_detections
            else:
                # 非推理帧：使用缓存结果
                phone_detections = last_phone_detections
                pc_detections = last_pc_detections
                all_detections = last_all_detections

            # 是否检测到手机
            phone_detected = len(phone_detections) > 0

            # 手部接触判断：手机场景
            phone_hand_touching = False
            phone_contact_points = []
            if phone_detected:
                primary_phone = max(phone_detections, key=lambda d: d['score'])
                phone_hand_touching, phone_contact_points = check_hand_touching_phone(
                    hand_landmarks, primary_phone['bbox'], w, h
                )

            # 手部接触判断：电脑场景（任一手接触鼠标或键盘）
            pc_detected = len(pc_detections) > 0
            pc_hand_touching = False
            pc_contact_points = []
            if pc_detected:
                for det in pc_detections:
                    touching, points = check_hand_touching_phone(
                        hand_landmarks, det['bbox'], w, h
                    )
                    if touching:
                        pc_hand_touching = True
                        pc_contact_points.extend(points)

            # 合并接触点用于显示
            contact_points = phone_contact_points + pc_contact_points

            # 触发逻辑：手机场景或电脑场景任一满足
            trigger_source = None
            if phone_detected and phone_hand_touching:
                trigger_source = "手机"
            elif pc_detected and pc_hand_touching:
                trigger_source = "电脑"

            current_time = time.time()
            should_trigger = (
                trigger_source is not None and
                current_time - last_trigger_time > TRIGGER_COOLDOWN and
                not verifier.is_processing
            )

            if should_trigger:
                last_trigger_time = current_time
                verifier.verify(frame.copy())
                print(f"[触发] {time.strftime('%H:%M:%S')} [{trigger_source}场景] 手部接触，发送API验证...")

            # 获取API结果
            api_result, api_reason = verifier.get_result()

            # 显示
            display = frame.copy()

            # 绘制所有检测结果（调试模式）
            if show_debug and all_detections:
                for det in all_detections:
                    x, y, w, h = det['bbox']
                    name = det['name']
                    score = det['score']

                    if name == "cell phone":
                        color = (0, 0, 255)
                        cv2.rectangle(display, (x, y), (x + w, y + h), color, 2)
                        label = f"{name} {score:.2f}"
                        cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), color, -1)
                        cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
                    elif name in ("mouse", "keyboard"):
                        color = (255, 0, 0) if name == "mouse" else (255, 165, 0)
                        cv2.rectangle(display, (x, y), (x + w, y + h), color, 2)
                        label = f"{name} {score:.2f}"
                        cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), color, -1)
                        cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
                    else:
                        color = (100, 100, 100)
                        cv2.rectangle(display, (x, y), (x + w, y + h), color, 1)

            # 也绘制非推理帧的检测（保持上一帧结果）
            if phone_detections:
                for det in phone_detections:
                    x, y, w, h = det['bbox']
                    cv2.rectangle(display, (x, y), (x + w, y + h), (0, 0, 255), 2)
                    label = f"phone {det['score']:.2f}"
                    cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), (0, 0, 255), -1)
                    cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

            if pc_detections:
                for det in pc_detections:
                    x, y, w, h = det['bbox']
                    color = (255, 0, 0) if det['name'] == 'mouse' else (255, 165, 0)
                    cv2.rectangle(display, (x, y), (x + w, y + h), color, 2)
                    label = f"{det['name']} {det['score']:.2f}"
                    cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), color, -1)
                    cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

            # 绘制手部骨架
            if show_debug and hand_landmarks:
                hand_detector.draw(display)

            # 绘制接触点（黄色标记）
            if contact_points:
                for px, py in contact_points:
                    cv2.circle(display, (px, py), 6, (0, 255, 255), -1)
                    cv2.circle(display, (px, py), 6, (0, 0, 0), 1)

            # 顶部状态栏（增加高度容纳电脑状态）
            overlay = display.copy()
            cv2.rectangle(overlay, (0, 0), (display.shape[1], 140), (0, 0, 0), -1)
            display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

            # 手机场景状态
            phone_color = (0, 255, 0) if phone_hand_touching else ((0, 255, 255) if phone_detected else (100, 100, 100))
            phone_text = f"手机: {'接触' if phone_hand_touching else ('检测到' if phone_detected else '未检测')}"
            if phone_detections:
                phone_text += f" ({phone_detections[0]['score']:.2f})"
            display = put_chinese_text(display, phone_text, (10, 8), font_small, phone_color)

            # 电脑场景状态
            pc_color = (0, 255, 0) if pc_hand_touching else ((0, 255, 255) if pc_detected else (100, 100, 100))
            pc_text = f"电脑: {'接触' if pc_hand_touching else ('检测到' if pc_detected else '未检测')}"
            if pc_detections:
                names = [d['name'] for d in pc_detections]
                pc_text += f" ({'+'.join(set(names))})"
            display = put_chinese_text(display, pc_text, (10, 30), font_small, pc_color)

            # 手部状态
            hand_touching_any = phone_hand_touching or pc_hand_touching
            hand_color = (0, 255, 0) if hand_touching_any else ((0, 255, 255) if hand_landmarks else (100, 100, 100))
            hand_status = f"手部: {'接触' if hand_touching_any else ('已检测' if hand_landmarks else '未检测')}"
            display = put_chinese_text(display, hand_status, (10, 52), font_small, hand_color)

            # API状态
            api_status = f"API: {'验证中' if verifier.is_processing else '空闲'}"
            if verifier.is_processing:
                cooldown = max(0, TRIGGER_COOLDOWN - (current_time - last_trigger_time))
                api_status += f" | 冷却: {cooldown:.0f}s"
            display = put_chinese_text(display, api_status, (10, 74), font_small, (255, 255, 255))

            # API结果（右上角显示，右对齐）
            if api_result:
                color_map = {"是": (0, 0, 255), "否": (0, 255, 0), "不确定": (0, 255, 255)}
                result_color = color_map.get(api_result, (200, 200, 200))
                result_text = f"结果: {api_result}"
                result_w = font_medium.getlength(result_text)
                display = put_chinese_text(display, result_text,
                                            (display.shape[1] - int(result_w) - 10, 8),
                                            font_medium, result_color)
                if api_reason:
                    # 多行显示理由（每行最多 25 字符，最多 3 行），右对齐
                    reason_lines = [api_reason[i:i+25] for i in range(0, len(api_reason), 25)][:3]
                    for i, line in enumerate(reason_lines):
                        prefix = "理由: " if i == 0 else ""
                        full_line = f"{prefix}{line}"
                        line_w = font_small.getlength(full_line)
                        display = put_chinese_text(display, full_line,
                                                    (display.shape[1] - int(line_w) - 10, 8 + 22 + i * 18),
                                                    font_small, (255, 255, 255))

            cv2.imshow("Focus Detector (Phone + PC)", display)

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
        hand_detector.close()
        cap.release()
        cv2.destroyAllWindows()
        print("资源已释放")


if __name__ == "__main__":
    main()
