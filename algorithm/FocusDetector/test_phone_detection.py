"""
玩手机检测 MVP - 本地预筛 + 豆包API验证

触发逻辑：
1. MediaPipe 检测手部关键点
2. 判断"手机握持姿态"（拇指食指接近，其余手指弯曲）
3. 在手部 ROI 内检测矩形反光物体（手机屏幕）
4. 两个条件同时满足 → 发送图片给豆包API验证
5. 豆包API返回"是否在玩游戏"

操作：
- 按 'q' 退出
- 按 's' 手动触发API调用（跳过本地预筛）
- 按 'd' 切换调试信息显示
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

# 握持姿态参数
THUMB_INDEX_DIST_MAX = 0.15    # 拇指尖与食指尖距离阈值（归一化）
FINGER_CURL_RATIO_MIN = 0.5    # 手指弯曲判定：指尖y/指根y > 此值视为弯曲

# 矩形检测参数
MIN_RECT_AREA = 1500           # 最小矩形面积（像素）
MAX_RECT_AREA = 60000          # 最大矩形面积
RECT_FILL_RATIO_MIN = 0.4      # 矩形填充比（轮廓面积/外接矩形面积）
BRIGHTNESS_THRESHOLD = 80      # 屏幕亮度阈值（均值）

# 触发冷却
TRIGGER_COOLDOWN = 5.0         # 触发后冷却时间（秒）
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


class GestureDetector:
    """MediaPipe 手势检测器"""

    def __init__(self, model_path="checkpoint/gesture_recognizer.task"):
        self.gesture_category = None
        self.hand_landmarks = []
        self.handedness = None

        def save_result(result, unused_output_image, timestamp_ms):
            try:
                self.gesture_category = None
                self.hand_landmarks = []
                self.handedness = None

                if result.gestures and result.gestures[0]:
                    self.gesture_category = result.gestures[0][0].category_name

                if result.hand_landmarks:
                    for hand in result.hand_landmarks:
                        self.hand_landmarks = [(lm.x, lm.y) for lm in hand]

                if result.handedness and result.handedness[0]:
                    self.handedness = result.handedness[0][0].category_name
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
            'handedness': self.handedness,
        }

    def close(self):
        self.recognizer.close()


def detect_phone_grip(landmarks):
    """
    判断手机握持姿态。

    判断条件：
    1. 拇指尖(4)与食指尖(8)距离接近
    2. 中指(12)、无名指(16)、小指(20)弯曲（指尖y > PIP关节y）

    返回: (is_grip: bool, details: dict)
    """
    if not landmarks or len(landmarks) < 21:
        return False, {}

    # 1. 拇指尖(4)与食指尖(8)距离
    thumb_tip = landmarks[4]
    index_tip = landmarks[8]
    thumb_index_dist = np.sqrt((thumb_tip[0] - index_tip[0])**2 + (thumb_tip[1] - index_tip[1])**2)

    # 2. 中指、无名指、小指弯曲判断
    # 弯曲：指尖y > PIP关节y（画面坐标y向下为正，指尖在关节下方=弯曲）
    # 但握手机时手指可能向前弯曲，用指尖与手腕的y差判断更稳定
    wrist = landmarks[0]

    curl_info = {}
    curled_count = 0
    for name, tip_idx, pip_idx in [("middle", 12, 10), ("ring", 16, 14), ("pinky", 20, 18)]:
        tip = landmarks[tip_idx]
        pip = landmarks[pip_idx]
        # 指尖相对PIP的y差：正值=指尖在PIP下方=弯曲
        curl = tip[1] - pip[1]
        curl_info[name] = {"curl": curl, "is_curled": curl > 0.02}
        if curl > 0.02:
            curled_count += 1

    # 食指：稍微弯曲（不完全握拳，但也不是完全伸直）
    index_tip = landmarks[8]
    index_pip = landmarks[6]
    index_curl = index_tip[1] - index_pip[1]
    index_semi_curled = -0.02 < index_curl < 0.15

    is_grip = (
        thumb_index_dist < THUMB_INDEX_DIST_MAX and
        curled_count >= 2 and  # 至少2根手指弯曲
        index_semi_curled      # 食指半弯曲
    )

    return is_grip, {
        "thumb_index_dist": thumb_index_dist,
        "curled_count": curled_count,
        "index_curl": index_curl,
        "curl_info": curl_info,
    }


def detect_rectangular_screen(frame, hand_landmarks, debug=False):
    """
    在手部 ROI 内检测矩形反光物体（手机屏幕）。

    返回: (found: bool, rect: tuple|None, debug_img: np.ndarray)
        rect: (x, y, w, h) 矩形位置
    """
    if not hand_landmarks or len(hand_landmarks) < 21:
        return False, None, frame

    h, w = frame.shape[:2]

    # 计算手部 ROI：以手腕(0)和各指尖为中心，扩大一定范围
    all_x = [lm[0] * w for lm in hand_landmarks]
    all_y = [lm[1] * h for lm in hand_landmarks]

    min_x = max(0, int(min(all_x) - 30))
    max_x = min(w, int(max(all_x) + 30))
    min_y = max(0, int(min(all_y) - 30))
    max_y = min(h, int(max(all_y) + 30))

    if max_x - min_x < 20 or max_y - min_y < 20:
        return False, None, frame

    roi = frame[min_y:max_y, min_x:max_x]
    debug_img = frame.copy()

    # 在 ROI 上做边缘检测
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)

    # 亮度检查：手机屏幕通常较亮
    mean_brightness = np.mean(gray)

    # Canny 边缘检测
    edges = cv2.Canny(gray, 50, 150)

    # 查找轮廓
    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    best_rect = None
    best_score = 0

    for cnt in contours:
        area = cv2.contourArea(cnt)
        if area < MIN_RECT_AREA or area > MAX_RECT_AREA:
            continue

        # 多边形近似
        epsilon = 0.04 * cv2.arcLength(cnt, True)
        approx = cv2.approxPolyDP(cnt, epsilon, True)

        # 矩形：4个顶点
        if len(approx) >= 4 and len(approx) <= 6:
            x, y, rw, rh = cv2.boundingRect(approx)
            rect_area = rw * rh
            fill_ratio = area / rect_area if rect_area > 0 else 0

            # 填充比检查
            if fill_ratio < RECT_FILL_RATIO_MIN:
                continue

            # 长宽比检查（手机屏幕通常长宽比在1.5-3之间）
            aspect = max(rw, rh) / max(min(rw, rh), 1)
            if aspect < 1.2 or aspect > 4.0:
                continue

            # 亮度检查
            roi_rect = gray[y:y+rh, x:x+rw]
            rect_brightness = np.mean(roi_rect) if roi_rect.size > 0 else 0

            # 综合评分
            score = fill_ratio * (1 + rect_brightness / 255)
            if score > best_score:
                best_score = score
                best_rect = (min_x + x, min_y + y, rw, rh)

            if debug:
                # 在 debug_img 上绘制候选矩形
                cv2.rectangle(debug_img, (min_x + x, min_y + y),
                               (min_x + x + rw, min_y + y + rh), (0, 255, 255), 2)

    found = best_rect is not None and mean_brightness > BRIGHTNESS_THRESHOLD

    if debug and best_rect:
        rx, ry, rw, rh = best_rect
        cv2.rectangle(debug_img, (rx, ry), (rx + rw, ry + rh), (0, 0, 255), 3)

    # 在 debug_img 上绘制 ROI 边界
    if debug:
        cv2.rectangle(debug_img, (min_x, min_y), (max_x, max_y), (100, 100, 100), 1)

    return found, best_rect, debug_img


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
    print("玩手机检测 MVP - 本地预筛 + 豆包API验证")
    print("=" * 60)
    print("触发条件：握持姿态 + ROI内矩形屏幕检测")
    print("按 'q' 退出 | 's' 手动触发API | 'd' 切换调试显示")
    print("=" * 60)

    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"错误：无法打开摄像头 (index={CAMERA_INDEX})")
        return

    base_dir = os.path.dirname(os.path.abspath(__file__))
    gesture_detector = GestureDetector(
        model_path=os.path.join(base_dir, "checkpoint", "gesture_recognizer.task")
    )
    verifier = DoubaoVerifier()

    last_trigger_time = 0
    show_debug = True

    font_medium = _find_chinese_font(18)
    font_small = _find_chinese_font(14)
    font_tiny = _find_chinese_font(12)

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

            hand_landmarks = gesture_result['hand_landmarks']

            # 1. 握持姿态检测
            is_grip, grip_info = detect_phone_grip(hand_landmarks)

            # 2. 矩形屏幕检测
            has_screen, screen_rect, debug_frame = detect_rectangular_screen(
                frame, hand_landmarks, debug=show_debug
            )

            # 3. 组合触发判断
            current_time = time.time()
            should_trigger = (
                is_grip and has_screen and
                current_time - last_trigger_time > TRIGGER_COOLDOWN and
                not verifier.is_processing
            )

            if should_trigger:
                last_trigger_time = current_time
                verifier.verify(frame.copy())
                print(f"[触发] {time.strftime('%H:%M:%S')} 握持+矩形检测通过，发送API验证...")

            # 获取API结果
            api_result, api_reason = verifier.get_result()

            # 显示
            display = debug_frame.copy()

            # 顶部状态栏：半透明背景
            overlay = display.copy()
            cv2.rectangle(overlay, (0, 0), (display.shape[1], 90), (0, 0, 0), -1)
            display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

            # 状态指示
            grip_color = (0, 255, 0) if is_grip else (100, 100, 100)
            screen_color = (0, 255, 0) if has_screen else (100, 100, 100)
            trigger_color = (0, 0, 255) if should_trigger else (200, 200, 200)

            display = put_chinese_text(display, f"握持: {'是' if is_grip else '否'}", (10, 8), font_small, grip_color)
            display = put_chinese_text(display, f"屏幕: {'是' if has_screen else '否'}", (130, 8), font_small, screen_color)

            cooldown_left = max(0, TRIGGER_COOLDOWN - (current_time - last_trigger_time))
            status_text = f"API: {'验证中' if verifier.is_processing else '空闲'}"
            if verifier.is_processing:
                status_text += f" | 冷却: {cooldown_left:.0f}s"
            display = put_chinese_text(display, status_text, (250, 8), font_small, (255, 255, 255))

            # API结果
            if api_result:
                color_map = {"是": (0, 0, 255), "否": (0, 255, 0), "不确定": (0, 255, 255)}
                result_color = color_map.get(api_result, (200, 200, 200))
                display = put_chinese_text(display, f"结果: {api_result}", (10, 32), font_medium, result_color)
                if api_reason:
                    display = put_chinese_text(display, f"理由: {api_reason[:25]}", (10, 58), font_small, (255, 255, 255))

            # 调试信息
            if show_debug and hand_landmarks:
                # 绘制手部关键点
                h, w = frame.shape[:2]
                for i, (x, y) in enumerate(hand_landmarks):
                    px, py = int(x * w), int(y * h)
                    cv2.circle(display, (px, py), 3, (0, 255, 255), -1)

                # 调试文字（右下角）
                dbg_lines = [
                    f"拇指食指距: {grip_info.get('thumb_index_dist', 0):.3f}",
                    f"弯曲指数: {grip_info.get('curled_count', 0)}/3",
                ]
                for i, line in enumerate(dbg_lines):
                    display = put_chinese_text(display, line,
                        (display.shape[1] - 200, display.shape[0] - 40 + i * 16),
                        font_tiny, (180, 180, 180))

            cv2.imshow("Phone Game Detector", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('s'):
                # 手动触发API
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
        gesture_detector.close()
        print("资源已释放")


if __name__ == "__main__":
    main()
