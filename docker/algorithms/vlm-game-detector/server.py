"""
VLM 游戏检测服务（豆包 Doubao API + YOLO + MediaPipe 手部接触检测）

功能：
1. YOLOv8 检测画面中的 phone / mouse / keyboard（COCO 类别）
2. MediaPipe HandLandmarker 检测手部关键点
3. 判断手部关键点是否在目标 bbox 内（接触检测）
4. 手机/电脑场景任一接触 → 触发豆包 API 验证是否在玩游戏
5. 5 秒冷却防止重复触发
6. 返回可视化图像（YOLO 检测框 + 接触点 + 状态栏 + API 结果）

端点：
- GET  /health            健康检查
- POST /detect            仅返回检测结果
- POST /detect_visualize  返回结果 + 可视化图像
- POST /reset             重置状态
"""

import os
import time
import logging
import base64
import threading
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont
from flask import Flask, request, jsonify
from openai import OpenAI
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision
from ultralytics import YOLO
from _common.common import decode_image_b64, find_chinese_font, put_chinese_text

logger = logging.getLogger('vlm_game_detector')

# ============ 配置 ============
API_KEY = os.getenv("ARK_API_KEY", "")  # 通过环境变量注入，勿硬编码
MODEL_ENDPOINT = os.getenv("VLM_MODEL_ENDPOINT", "doubao-seed-2-0-mini-260428")
BASE_URL = os.getenv("VLM_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3")
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "50"))
MAX_IMAGE_WIDTH = int(os.getenv("MAX_IMAGE_WIDTH", "640"))
PORT = int(os.getenv("PORT", "9014"))

# YOLOv8 参数
YOLO_MODEL = os.getenv("YOLO_MODEL", "yolo26s.pt")
PHONE_CLASS_ID = 67                  # COCO 0-indexed: cell phone
MOUSE_CLASS_ID = 64                  # COCO 0-indexed: mouse
KEYBOARD_CLASS_ID = 66               # COCO 0-indexed: keyboard
PHONE_CONFIDENCE_THRESHOLD = float(os.getenv("PHONE_CONFIDENCE_THRESHOLD", "0.2"))
PC_CONFIDENCE_THRESHOLD = float(os.getenv("PC_CONFIDENCE_THRESHOLD", "0.15"))
TRIGGER_COOLDOWN = float(os.getenv("TRIGGER_COOLDOWN", "5.0"))

# MediaPipe 手部参数
HAND_LANDMARKER_PATH = os.getenv("HAND_LANDMARKER_PATH", "/app/hand_landmarker.task")
HAND_CONTACT_MARGIN = int(os.getenv("HAND_CONTACT_MARGIN", "25"))
# 手部关键点索引：WRIST=0, INDEX_FINGER_MCP=5, MIDDLE_FINGER_MCP=9, RING_FINGER_MCP=13, PINKY_MCP=17
HAND_KEY_LANDMARKS = [0, 5, 9, 13, 17]  # 手腕、食指根、中指根、无名指根、小指根
HAND_MIN_DETECTION_CONFIDENCE = float(os.getenv("HAND_MIN_DETECTION_CONFIDENCE", "0.3"))
HAND_MIN_PRESENCE_CONFIDENCE = float(os.getenv("HAND_MIN_PRESENCE_CONFIDENCE", "0.3"))
HAND_MIN_TRACKING_CONFIDENCE = float(os.getenv("HAND_MIN_TRACKING_CONFIDENCE", "0.5"))
# ==============================

app = Flask(__name__)

SYSTEM_PROMPT = """你是一个行为分析助手。请观察这张图片中的人，判断他/她是否在玩游戏（包括电脑游戏和手机游戏）。

判断依据：
- 正在玩游戏：屏幕显示游戏画面、手持游戏手柄、键盘快速操作、明显的游戏界面、手持手机玩游戏（横屏或竖屏游戏画面）、低头专注手机屏幕且手指快速滑动
- 不是玩游戏：正常办公、浏览网页、看视频、写代码、刷社交媒体、打电话等日常使用

请严格按以下格式回答，不要输出其他内容：
判断：是/否/不确定
理由：一句话说明"""


def is_landmark_in_bbox(lm_x, lm_y, bbox, margin=0):
    """判断关键点是否在 bbox 内（含边缘容差）"""
    x, y, w, h = bbox
    return (x - margin) <= lm_x <= (x + w + margin) and (y - margin) <= lm_y <= (y + h + margin)


def check_hand_touching(hand_landmarks_list, target_bbox, img_w, img_h):
    """
    判断手部是否接触目标 bbox。
    hand_landmarks_list: MediaPipe HandLandmarker 返回的 hand_landmarks
    target_bbox: (x, y, w, h)
    返回: (是否接触, 接触点坐标列表)
    """
    if not hand_landmarks_list:
        return False, []

    contact_points = []
    for hand_lms in hand_landmarks_list:
        landmarks = hand_lms.landmark if hasattr(hand_lms, 'landmark') else hand_lms
        for idx in HAND_KEY_LANDMARKS:
            if idx < len(landmarks):
                lm = landmarks[idx]
                px, py = int(lm.x * img_w), int(lm.y * img_h)
                if is_landmark_in_bbox(px, py, target_bbox, HAND_CONTACT_MARGIN):
                    contact_points.append((px, py))

    return len(contact_points) > 0, contact_points


# ============ 模型加载 ============
yolo_model = None
hand_landmarker = None

# 手部骨架连接关系（用于可视化）
HAND_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4),
    (0, 5), (5, 6), (6, 7), (7, 8),
    (5, 9), (9, 10), (10, 11), (11, 12),
    (9, 13), (13, 14), (14, 15), (15, 16),
    (13, 17), (0, 17), (17, 18), (18, 19), (19, 20),
]


def load_yolo_model():
    """加载 YOLOv8 模型（首次运行会自动下载）"""
    global yolo_model
    if yolo_model is None:
        try:
            yolo_model = YOLO(YOLO_MODEL)
            logger.info('YOLO model loaded: %s', YOLO_MODEL)
        except Exception as e:
            logger.error('Failed to load YOLO model: %s', e)


def load_hand_landmarker():
    """加载 MediaPipe HandLandmarker"""
    global hand_landmarker
    if hand_landmarker is None:
        try:
            base_options = mp_python.BaseOptions(model_asset_path=HAND_LANDMARKER_PATH)
            options = mp_vision.HandLandmarkerOptions(
                base_options=base_options,
                running_mode=mp_vision.RunningMode.IMAGE,
                num_hands=2,
                min_hand_detection_confidence=HAND_MIN_DETECTION_CONFIDENCE,
                min_hand_presence_confidence=HAND_MIN_PRESENCE_CONFIDENCE,
                min_tracking_confidence=HAND_MIN_TRACKING_CONFIDENCE,
            )
            hand_landmarker = mp_vision.HandLandmarker.create_from_options(options)
            logger.info('HandLandmarker loaded: %s', HAND_LANDMARKER_PATH)
        except Exception as e:
            logger.error('Failed to load HandLandmarker: %s', e)


# 可视化布局参数
VIS_STATUS_BAR_HEIGHT = 140       # 顶部状态栏高度
VIS_BOTTOM_BAR_HEIGHT = 28        # 底部状态栏高度
VIS_OVERLAY_ALPHA = 0.6           # 状态栏透明度
VIS_REASON_MAX_CHARS = 25         # 理由每行最大字符
VIS_REASON_MAX_LINES = 3          # 理由最大行数

# API 结果解析前缀
JUDGMENT_PREFIX_CN = '判断：'
REASON_PREFIX_CN = '理由：'

# 中文判断 → 英文枚举（跨端协议：回传 ESP32 / 前端使用英文枚举）
JUDGMENT_EN_MAP = {"是": "yes", "否": "no", "不确定": "uncertain"}


class GameDetectorEngine:
    """游戏检测引擎：YOLO + 手部接触 + 豆包 API"""

    @staticmethod
    def decode_image_from_request(data: dict):
        """从请求数据解码图像，返回 (frame, error_response)
        frame: 解码后的 BGR 图像（成功时）或 None（失败时）
        error_response: Flask 错误响应（失败时）或 None（成功时）
        """
        image_b64 = data.get('image')
        if not image_b64:
            return None, (jsonify({"success": False, "error": "No image provided"}), 400)
        frame = decode_image_b64(image_b64)
        if frame is None:
            return None, (jsonify({"success": False, "error": "Invalid image"}), 400)
        return frame, None

    def __init__(self):
        self.client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
        self.last_judgment = "pending"
        self.last_reason = ""
        self.last_timestamp = 0
        self.is_processing = False
        self.lock = threading.Lock()
        self.last_trigger_time = 0.0
        # 缓存上次的检测状态（用于非触发帧的可视化）
        self.last_phone_detections = []
        self.last_pc_detections = []
        self.last_hand_landmarks = []
        self.last_phone_touching = False
        self.last_pc_touching = False
        self.last_contact_points = []
        self.last_trigger_source = None
        # 推理锁：YOLO + HandLandmarker 推理互斥，避免并发请求同时抢 CPU
        self.infer_lock = threading.Lock()
        # 字体
        self.font_large = find_chinese_font(24)
        self.font_medium = find_chinese_font(18)
        self.font_small = find_chinese_font(14)

    def detect_yolo_hand(self, frame_bgr):
        """YOLO + 手部检测（不调用豆包 API）——串行推理，防止并发竞争"""
        with self.infer_lock:
            return self._detect_yolo_hand_locked(frame_bgr)

    def _detect_yolo_hand_locked(self, frame_bgr):
        h, w = frame_bgr.shape[:2]

        # 1. YOLOv8 检测 phone / mouse / keyboard
        phone_detections = []
        pc_detections = []
        if yolo_model is not None:
            try:
                target_classes = [PHONE_CLASS_ID, MOUSE_CLASS_ID, KEYBOARD_CLASS_ID]
                results = yolo_model(frame_bgr, verbose=False, classes=target_classes, conf=0.1)
                for result in results:
                    for box in result.boxes:
                        cls_id = int(box.cls[0])
                        conf = float(box.conf[0])
                        x1, y1, x2, y2 = box.xyxy[0].tolist()
                        x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
                        bbox = (x1, y1, x2 - x1, y2 - y1)
                        if cls_id == PHONE_CLASS_ID and conf >= PHONE_CONFIDENCE_THRESHOLD:
                            phone_detections.append({'bbox': bbox, 'score': conf})
                        elif cls_id in (MOUSE_CLASS_ID, KEYBOARD_CLASS_ID) and conf >= PC_CONFIDENCE_THRESHOLD:
                            pc_detections.append({
                                'bbox': bbox, 'score': conf,
                                'name': 'mouse' if cls_id == MOUSE_CLASS_ID else 'keyboard',
                            })
            except Exception as e:
                logger.warning('YOLO inference failed: %s', e)
        else:
            pass

        # 2. MediaPipe 手部检测
        hand_landmarks_list = []
        if hand_landmarker is not None:
            try:
                rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
                mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
                result = hand_landmarker.detect(mp_image)
                if result.hand_landmarks:
                    hand_landmarks_list = result.hand_landmarks
            except Exception as e:
                logger.warning('HandLandmarker failed: %s', e)

        # 3. 接触判断
        phone_detected = len(phone_detections) > 0
        pc_detected = len(pc_detections) > 0
        phone_hand_touching = False
        pc_hand_touching = False
        contact_points = []

        if phone_detected:
            primary_phone = max(phone_detections, key=lambda d: d['score'])
            phone_hand_touching, phone_pts = check_hand_touching(
                hand_landmarks_list, primary_phone['bbox'], w, h
            )
            contact_points.extend(phone_pts)

        if pc_detected:
            for det in pc_detections:
                touching, pts = check_hand_touching(
                    hand_landmarks_list, det['bbox'], w, h
                )
                if touching:
                    pc_hand_touching = True
                    contact_points.extend(pts)

        # 4. 触发源
        trigger_source = None
        if phone_detected and phone_hand_touching:
            trigger_source = "phone"
        elif pc_detected and pc_hand_touching:
            trigger_source = "computer"

        # 5. 缓存状态
        with self.lock:
            self.last_phone_detections = phone_detections
            self.last_pc_detections = pc_detections
            self.last_hand_landmarks = hand_landmarks_list
            self.last_phone_touching = phone_hand_touching
            self.last_pc_touching = pc_hand_touching
            self.last_contact_points = contact_points
            self.last_trigger_source = trigger_source

        return {
            'phone_detected': phone_detected,
            'pc_detected': pc_detected,
            'phone_hand_touching': phone_hand_touching,
            'pc_hand_touching': pc_hand_touching,
            'trigger_source': trigger_source,
            'phone_detections': phone_detections,
            'pc_detections': pc_detections,
            'hand_detected': len(hand_landmarks_list) > 0,
            'contact_points': contact_points,
        }

    def should_trigger_api(self, trigger_source):
        """判断是否应该触发豆包 API"""
        current_time = time.time()
        with self.lock:
            cooldown_remaining = max(0.0, TRIGGER_COOLDOWN - (current_time - self.last_trigger_time))
            should = (
                trigger_source is not None and
                cooldown_remaining <= 0 and
                not self.is_processing
            )
            return should, cooldown_remaining

    def call_doubao_api(self, frame_bgr):
        """调用豆包 API"""
        h, w = frame_bgr.shape[:2]
        if w > MAX_IMAGE_WIDTH:
            scale = MAX_IMAGE_WIDTH / w
            frame_bgr = cv2.resize(frame_bgr, (MAX_IMAGE_WIDTH, int(h * scale)))

        _, buffer = cv2.imencode('.jpg', frame_bgr, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
        b64_image = base64.b64encode(buffer).decode('utf-8')
        data_uri = f"data:image/jpeg;base64,{b64_image}"

        response = self.client.responses.create(
            model=MODEL_ENDPOINT,
            input=[
                {
                    "role": "user",
                    "content": [
                        {"type": "input_image", "image_url": data_uri},
                        {"type": "input_text", "text": SYSTEM_PROMPT},
                    ],
                }
            ],
            extra_body={"thinking": {"type": "disabled"}},
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

        # 提取判断和理由（中文 → 英文枚举：是/否/不确定 → yes/no/uncertain）
        judgment = "uncertain"
        reason = ""
        for line in result_text.split('\n'):
            line = line.strip()
            if line.startswith(JUDGMENT_PREFIX_CN) or line.startswith('判断:'):
                raw = line.split('：', 1)[-1].split(':', 1)[-1].strip()
                judgment = JUDGMENT_EN_MAP.get(raw, "uncertain")
            elif line.startswith(REASON_PREFIX_CN) or line.startswith('理由:'):
                reason = line.split('：', 1)[-1].split(':', 1)[-1].strip()

        return judgment, reason

    def detect_and_maybe_trigger_api(self, frame_bgr):
        """
        同步执行 YOLO + 手部检测，条件满足时异步触发豆包 API。

        Returns:
            dict: 包含以下字段：
                - detection: YOLO+手部检测结果
                - judgment: 上次 AI 判断结果
                - reason: 上次 AI 理由
                - api_triggered: 本次是否触发了 API
                - cooldown_remaining: 冷却剩余秒数
                - is_processing: API 是否正在调用中
                - timestamp: 时间戳
                - processing_time: 本帧处理耗时

        Note: 豆包 API 在后台线程执行，不阻塞当前请求
        """
        t0 = time.time()

        # 1. YOLO + 手部检测
        detection = self.detect_yolo_hand(frame_bgr)
        trigger_source = detection['trigger_source']

        # 2. 判断是否触发豆包 API
        should_call_api, cooldown_remaining = self.should_trigger_api(trigger_source)

        api_triggered = False
        # 3. 如果需要，异步触发豆包 API（不阻塞当前请求）
        if should_call_api:
            with self.lock:
                self.is_processing = True
                self.last_trigger_time = time.time()
            api_triggered = True
            # 启动后台线程调用豆包 API
            thread = threading.Thread(
                target=self._call_doubao_api_async,
                args=(frame_bgr.copy(),),
                daemon=True
            )
            thread.start()
            logger.info('VLM-Game: 触发源=%s，异步调用豆包 API', trigger_source)

        # 4. 返回当前检测结果 + 上次 API 结果
        with self.lock:
            judgment = self.last_judgment
            reason = self.last_reason
            is_processing = self.is_processing

        elapsed = time.time() - t0

        return {
            'detection': detection,
            'judgment': judgment,
            'reason': reason,
            'api_triggered': api_triggered,
            'cooldown_remaining': cooldown_remaining,
            'is_processing': is_processing,
            'timestamp': int(time.time() * 1000),
            'processing_time': elapsed,
        }

    def _call_doubao_api_async(self, frame_bgr):
        """后台线程：调用豆包 API"""
        try:
            judgment, reason = self.call_doubao_api(frame_bgr)
            with self.lock:
                self.last_judgment = judgment
                self.last_reason = reason
                self.last_timestamp = int(time.time() * 1000)
            logger.info('VLM-Game: API 调用完成: 判断=%s | 理由=%s', judgment, reason)
        except Exception as e:
            with self.lock:
                self.last_judgment = "error"
                self.last_reason = str(e)[:80]
                self.last_timestamp = int(time.time() * 1000)
            logger.error('VLM-Game: API 调用失败: %s', e)
        finally:
            with self.lock:
                self.is_processing = False

    def get_state(self):
        with self.lock:
            return {
                "judgment": self.last_judgment,
                "reason": self.last_reason,
                "timestamp": self.last_timestamp,
                "is_processing": self.is_processing,
                "last_trigger_source": self.last_trigger_source,
                "phone_touching": self.last_phone_touching,
                "pc_touching": self.last_pc_touching,
            }


engine = GameDetectorEngine()

# 模块级加载模型（gunicorn 以模块导入方式启动，必须在 import 阶段完成）
load_yolo_model()
load_hand_landmarker()

# 判断结果颜色映射（BGR）
JUDGMENT_COLORS = {
    "yes": (0, 0, 255),
    "no": (0, 255, 0),
    "uncertain": (0, 255, 255),
    "pending": (200, 200, 200),
    "error": (0, 0, 200),
}


def visualize_result(frame_bgr, result):
    """在画面上绘制 YOLO 检测框 + 接触点 + 状态栏 + API 结果"""
    display = frame_bgr.copy()
    h, w = display.shape[:2]

    detection = result.get('detection', {})
    phone_dets = detection.get('phone_detections', [])
    pc_dets = detection.get('pc_detections', [])
    contact_points = detection.get('contact_points', [])
    phone_touching = detection.get('phone_hand_touching', False)
    pc_touching = detection.get('pc_hand_touching', False)
    phone_detected = detection.get('phone_detected', False)
    pc_detected = detection.get('pc_detected', False)
    hand_detected = detection.get('hand_detected', False)
    trigger_source = detection.get('trigger_source')

    judgment = result.get('judgment', 'pending')
    reason = result.get('reason', '')
    api_triggered = result.get('api_triggered', False)
    is_processing = result.get('is_processing', False)
    cooldown_remaining = result.get('cooldown_remaining', 0)

    # 1. 绘制 YOLO 检测框
    for det in phone_dets:
        x, y, bw, bh = det['bbox']
        cv2.rectangle(display, (x, y), (x + bw, y + bh), (0, 0, 255), 2)
        label = f"phone {det['score']:.2f}"
        cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), (0, 0, 255), -1)
        cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

    for det in pc_dets:
        x, y, bw, bh = det['bbox']
        color = (255, 0, 0) if det['name'] == 'mouse' else (255, 165, 0)
        cv2.rectangle(display, (x, y), (x + bw, y + bh), color, 2)
        label = f"{det['name']} {det['score']:.2f}"
        cv2.rectangle(display, (x, y - 20), (x + len(label) * 8, y), color, -1)
        cv2.putText(display, label, (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)

    # 2. 绘制手部骨架（如有）
    hand_landmarks_list = engine.last_hand_landmarks
    if hand_landmarks_list:
        for hand_lms in hand_landmarks_list:
            landmarks = hand_lms.landmark if hasattr(hand_lms, 'landmark') else hand_lms
            pts = [(int(lm.x * w), int(lm.y * h)) for lm in landmarks]
            for a, b in HAND_CONNECTIONS:
                if a < len(pts) and b < len(pts):
                    cv2.line(display, pts[a], pts[b], (255, 0, 0), 2)
            for px, py in pts:
                cv2.circle(display, (px, py), 3, (0, 0, 255), -1)

    # 3. 绘制接触点（黄色标记）
    for px, py in contact_points:
        cv2.circle(display, (px, py), 6, (0, 255, 255), -1)
        cv2.circle(display, (px, py), 6, (0, 0, 0), 1)

    # 4. 顶部状态栏
    overlay = display.copy()
    cv2.rectangle(overlay, (0, 0), (w, VIS_STATUS_BAR_HEIGHT), (0, 0, 0), -1)
    display = cv2.addWeighted(overlay, VIS_OVERLAY_ALPHA, display, 1 - VIS_OVERLAY_ALPHA, 0)

    # 手机场景状态
    phone_color = (0, 255, 0) if phone_touching else ((0, 255, 255) if phone_detected else (100, 100, 100))
    phone_text = f"手机: {'接触' if phone_touching else ('检测到' if phone_detected else '未检测')}"
    if phone_dets:
        phone_text += f" ({phone_dets[0]['score']:.2f})"
    display = put_chinese_text(display, phone_text, (10, 8), engine.font_small, phone_color)

    # 电脑场景状态
    pc_color = (0, 255, 0) if pc_touching else ((0, 255, 255) if pc_detected else (100, 100, 100))
    pc_text = f"电脑: {'接触' if pc_touching else ('检测到' if pc_detected else '未检测')}"
    if pc_dets:
        names = [d['name'] for d in pc_dets]
        pc_text += f" ({'+'.join(set(names))})"
    display = put_chinese_text(display, pc_text, (10, 30), engine.font_small, pc_color)

    # 手部状态
    hand_touching_any = phone_touching or pc_touching
    hand_color = (0, 255, 0) if hand_touching_any else ((0, 255, 255) if hand_detected else (100, 100, 100))
    hand_status = f"手部: {'接触' if hand_touching_any else ('已检测' if hand_detected else '未检测')}"
    display = put_chinese_text(display, hand_status, (10, 52), engine.font_small, hand_color)

    # API 状态
    if is_processing:
        api_status = "API: 验证中..."
    elif cooldown_remaining > 0:
        api_status = f"API: 冷却 {cooldown_remaining:.1f}s"
    else:
        api_status = f"API: 空闲 | 触发源: {trigger_source or '无'}"
    display = put_chinese_text(display, api_status, (10, 74), engine.font_small, (255, 255, 255))

    # 5. 右上角显示 API 结果
    if judgment and judgment != "pending":
        result_color = JUDGMENT_COLORS.get(judgment, (200, 200, 200))
        result_text = f"结果: {judgment}"
        text_w = engine.font_medium.getlength(result_text)
        display = put_chinese_text(display, result_text,
                                    (w - int(text_w) - 10, 8),
                                    engine.font_medium, result_color)
        if reason:
            # 多行显示理由（每行最多 25 字符，最多 3 行）
            reason_lines = [reason[i:i+VIS_REASON_MAX_CHARS] for i in range(0, len(reason), VIS_REASON_MAX_CHARS)][:VIS_REASON_MAX_LINES]
            for i, line in enumerate(reason_lines):
                prefix = "理由: " if i == 0 else ""
                full_line = f"{prefix}{line}"
                line_w = engine.font_small.getlength(full_line)
                display = put_chinese_text(display, full_line,
                                            (w - int(line_w) - 10, 8 + 22 + i * 18),
                                            engine.font_small, (255, 255, 255))

    # 6. 底部状态栏
    overlay2 = display.copy()
    cv2.rectangle(overlay2, (0, h - VIS_BOTTOM_BAR_HEIGHT), (w, h), (0, 0, 0), -1)
    display = cv2.addWeighted(overlay2, VIS_OVERLAY_ALPHA, display, 1 - VIS_OVERLAY_ALPHA, 0)
    status = "VLM-Game | 检测中..." if is_processing else "VLM-Game | YOLO+手部接触触发"
    display = put_chinese_text(display, status, (10, h - 22),
                               engine.font_small, (200, 200, 200))
    return display


@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "success": True,
        "service": "vlm-game-detector",
        "model": MODEL_ENDPOINT,
        "yolo_model": YOLO_MODEL,
        "processing": engine.is_processing,
    })


@app.route('/detect', methods=['POST'])
def detect():
    """同步检测：YOLO + 手部 + 接触 + 必要时调用豆包 API"""
    try:
        data = request.get_json()
        frame, error = GameDetectorEngine.decode_image_from_request(data)
        if error:
            return error

        result = engine.detect_and_maybe_trigger_api(frame)
        return jsonify({
            "success": True,
            "result": result,
            "processing_time": result['processing_time'],
        })
    except Exception as e:
        logger.error('/detect: %s', e)
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    """同步检测：返回结果 + 可视化图像"""
    try:
        data = request.get_json()
        frame, error = GameDetectorEngine.decode_image_from_request(data)
        if error:
            return error

        result = engine.detect_and_maybe_trigger_api(frame)

        # 生成可视化
        vis = visualize_result(frame, result)
        _, vis_buffer = cv2.imencode('.jpg', vis, [cv2.IMWRITE_JPEG_QUALITY, 80])
        vis_b64 = base64.b64encode(vis_buffer).decode('utf-8')

        return jsonify({
            "success": True,
            "result": result,
            "visualized_image": vis_b64,
            "processing_time": result['processing_time'],
        })
    except Exception as e:
        logger.error('/detect_visualize: %s', e)
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/state', methods=['GET'])
def state():
    """查询当前游戏检测引擎状态"""
    s = engine.get_state()
    return jsonify({
        "success": True,
        "result": s,
    })


@app.route('/reset', methods=['POST'])
def reset():
    with engine.lock:
        engine.last_judgment = "pending"
        engine.last_reason = ""
        engine.last_timestamp = 0
        engine.last_trigger_time = 0.0
    return jsonify({"success": True})


if __name__ == '__main__':
    logging.basicConfig(
        level=logging.INFO,
        format='[%(asctime)s] [%(name)s] [%(levelname)s] %(message)s',
        datefmt='%H:%M:%S'
    )
    logger.info('VLM Game Detector Service starting on port %s', PORT)
    logger.info('VLM Model: %s', MODEL_ENDPOINT)
    logger.info('YOLO Model: %s', YOLO_MODEL)
    logger.info('Base URL: %s', BASE_URL)
    logger.info('API Key configured: %s', 'YES' if API_KEY else 'NO')
    logger.info('Trigger cooldown: %ss', TRIGGER_COOLDOWN)
    app.run(host='0.0.0.0', port=PORT, threaded=True)
