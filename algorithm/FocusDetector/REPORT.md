# FocusDetector 项目复刻说明报告

> 生成日期：2026-07-04 | 版本：0.2.0

> 变更摘要（相对 0.1.0）：
> - 新增颈部保健操检测器、FaceLandmarker、面部空间坐标估计器
> - 重构：抽出 MediaPipeAsyncProvider 通用基类，消除三个 Provider 的重复模板代码
> - 重构：拆分 main.py 到 ui/ 模块（colors / draw / overlay）
> - 重构：统一 Detector 接口（close + __enter__/__exit__），使用 ExitStack 管理资源
> - 增强：FPS 监控、read_config_or_default 容错降级

---

## 1. 核心模块详细设计

### 1.1 情绪识别模块

**文件**：`detectors/emotion_detector/emotiefflib.py`

**算法**：
1. 使用 `facenet-pytorch` 的 MTCNN 进行人脸检测（`min_face_size=40`，置信度阈值 `0.9`）
2. 裁剪人脸区域
3. 使用 `emotiefflib` 的 `EmotiEffLibRecognizer`（ONNX 引擎）进行情绪分类

**输出接口**：
```python
def detect(self, image: np.ndarray) -> dict | None:
    # 返回格式：
    {'emotion': 'Happiness'}  # 或 None（无人脸）
```

**支持的 8 种情绪**：

| 情绪 | 英文名 |
|------|--------|
| 开心 | Happiness |
| 悲伤 | Sadness |
| 愤怒 | Anger |
| 恐惧 | Fear |
| 惊讶 | Surprise |
| 厌恶 | Disgust |
| 轻蔑 | Contempt |
| 中性 | Neutral |

**配置文件** (`config/emotion_detector.yaml`)：
```yaml
device: "cpu"
```

### 1.2 手势识别模块

**文件**：`detectors/gesture_detector/mediapipe.py`

**算法**：
1. 使用 MediaPipe `GestureRecognizer`（LIVE_STREAM 模式，异步回调）
2. 检测手势类别和手部 21 个关键点（归一化坐标 0.0~1.0）

**输出接口**：
```python
def run(self, image: np.ndarray):           # 异步提交帧
def get_detect_result(self) -> dict | None: # 获取最近结果
    # 返回格式：
    {
        'gesture': 'Open_Palm',             # 手势类别名
        'hand_landmarks': [                 # 手部关键点列表（每只手）
            [(x0, y0), (x1, y1), ...],      # 21 个关键点，归一化坐标
        ]
    }
    # 或 None（未检测到手）
```

**支持的 7 种手势**：

| 手势 | 英文名 |
|------|--------|
| 握拳 | Closed_Fist |
| 伸掌 | Open_Palm |
| 食指向上 | Pointing_Up |
| 拇指向下 | Thumb_Down |
| 拇指向上 | Thumb_Up |
| 剪刀手 | Victory |
| 我爱你 | ILoveYou |

**关键点索引**（21 个点）：

```
0:  手腕
1-4:  拇指（CMC→MCP→IP→TIP）
5-8:  食指（MCP→PIP→DIP→TIP）
9-12: 中指
13-16: 无名指
17-20: 小指
```

**配置文件** (`config/gesture_detector.yaml`)：
```yaml
model_path: "checkpoint/gesture_recognizer.task"
num_hands: 1
min_hand_detection_confidence: 0.5
min_hand_presence_confidence: 0.5
min_tracking_confidence: 0.5
```

### 1.3 组合手势检测模块

**文件**：`detectors/combo_gesture.py`

#### 1.3.1 设计演进（重要背景）

组合手势检测经历了多次迭代，理解演进过程对复刻至关重要：

**问题**：用户做"握拳→伸掌"动作时，MediaPipe 在过渡帧返回 `None` 或 `"None"`，导致手势序列被打断为 `Closed_Fist → None → Open_Palm`，无法匹配连续的 `(Closed_Fist, Open_Palm)` 组合。

**迭代过程**：

| 版本 | 方案 | 问题 |
|------|------|------|
| V1 | 连续帧状态机，要求相邻帧手势连续 | None 过渡帧打断序列 |
| V2 | 事件驱动 + transition_window 时间窗口 | 窗口过短导致自然手势超时 |
| V3 | 事件驱动 + hold_time 稳定性 + cooldown 冷却期 | hold_time 过长导致快速手势无法识别；cooldown 过长导致连续手势被阻断 |
| V4 | 事件驱动 + hold_time=0.1s + cooldown=0.3s | 仍有轻微延迟 |
| **V5（最终）** | **纯事件驱动，无 hold_time、无 cooldown** | 过渡帧抖动导致过度触发，但用户认可此效果 |

**最终方案核心思想**：

- **忽略 None 和 "None"**：`_INVALID_GESTURES = {None, "None"}`，过渡帧不参与判断
- **只记录最近一次有效手势**：当有效手势从 A 变为 B 时，查表 `(A, B)` 是否为已定义组合
- **无稳定性要求、无冷却期**：手势变化即触发，响应最快

#### 1.3.2 当前组合手势定义

```python
COMBO_GESTURES = {
    ("Closed_Fist", "Open_Palm"): "Open",       # 握拳→伸掌 = 开
    ("Open_Palm", "Closed_Fist"): "Close",      # 伸掌→握拳 = 关
    ("Closed_Fist", "Thumb_Up"): "VolumeUp",    # 握拳→竖拇指 = 音量+
    ("Closed_Fist", "Thumb_Down"): "VolumeDown", # 握拳→拇指向下 = 音量-
}
```

#### 1.3.3 核心算法伪代码

```
update(current_gesture):
    if current_gesture in {None, "None"}:
        return None                    # 忽略过渡帧

    if last_valid_gesture is None:     # 第一个有效手势
        last_valid_gesture = current_gesture
        return None

    if current_gesture == last_valid_gesture:  # 同一手势持续
        return None

    # 手势发生变化
    combo_name = COMBO_GESTURES.get((last_valid_gesture, current_gesture))
    last_valid_gesture = current_gesture       # 更新为当前手势
    return combo_name                          # 返回组合名称或 None
```

#### 1.3.4 调试日志

检测器内置调试日志，通过 `log_level: "DEBUG"` 配置启用，输出格式：

```
[12.87s] GESTURE_CHANGE: Open_Palm -> Closed_Fist
[12.87s] COMBO_TRIGGERED: Open_Palm -> Closed_Fist = Close
```

事件类型：
- `FIRST_GESTURE`：首次检测到有效手势
- `GESTURE_CHANGE`：有效手势发生变化
- `COMBO_TRIGGERED`：组合手势被触发
- `NO_COMBO_MATCH`：手势变化但无匹配组合

### 1.4 情绪+手势联动

**设计模式**：实时状态检测（非事件触发）

**核心逻辑**：每帧检查当前情绪和当前手势的组合是否匹配预定义联动，**任一条件不满足则立即返回 None**。

```python
EMOTION_GESTURE_COMBOS = {
    ("Happiness", "Open_Palm"): "Welcome",   # 开心+伸掌 = 欢迎
    ("Happiness", "Thumb_Up"): "Good",       # 开心+竖拇指 = 好
}
```

**与组合手势的区别**：

| 特性 | 组合手势 | 情绪联动 |
|------|----------|----------|
| 触发方式 | 事件驱动（手势变化时触发） | 实时状态（每帧检查） |
| 持续性 | 触发后保持到下一次触发 | 条件不满足立即消失 |
| 输入 | 手势序列 (A→B) | 当前情绪 + 当前手势 |

**实现位置**：在 `main.py` 主循环中直接实现，不在 `ComboGestureDetector` 中。

```python
# 情绪+手势联动（实时状态：只有当前帧同时满足才显示）
current_emotion = emotion_result['emotion'] if emotion_result else None
emotion_combo_key = (current_emotion, current_single_gesture)
emotion_combo_name = EMOTION_GESTURE_COMBOS.get(emotion_combo_key) if current_emotion and current_single_gesture else None
```

### 1.5 日志模块

**文件**：`utils/logger.py`

**关键设计**：使用 `_configured` 全局变量确保多模块调用 `setup_logging()` 时日志配置只初始化一次，避免后调用的模块覆盖日志级别。

```python
_configured = False

def setup_logging(log_level: str = "INFO"):
    global _configured
    if not _configured:
        logger.remove()
        logger.add(sys.stdout, format=log_format, level=log_level)
        _configured = True
    return logger
```

### 1.6 颈部保健操检测器（0.2.0 新增）

**文件**：`detectors/pose_detector/neck_exercise.py`

**算法**：基于 MediaPipe Pose 的 33 个关键点，识别颈部保健操动作并评分。

**支持动作**：
- 左右侧屈（Lateral Flexion）：头部向左右倾斜
- 前后屈伸（Flexion/Extension）：头部前低/后仰
- 左右旋转（Rotation）：头部向左右转动

**评分维度**：
- 幅度评分：动作角度是否达到标准范围（线性映射 `[min_angle, full_angle] -> [0, 100]`）
- 保持时间评分：最大角度处是否保持足够时间
- 综合评分：幅度 60% + 保持时间 40%

**输出接口**：
```python
def update(self, pose_landmarks: list | None) -> NeckAction:
    # NeckAction dataclass 字段：
    # name: str              # 动作名称（如 "Left_Lateral_Flexion"）
    # angle: float           # 当前角度（度）
    # amplitude_score: float # 幅度评分 0-100
    # hold_score: float      # 保持时间评分 0-100
    # total_score: float     # 综合评分 0-100
    # hold_duration: float   # 已保持时间（秒）
```

**动作阈值**：
```python
ACTION_THRESHOLDS = {
    "Left_Lateral_Flexion":  ActionThreshold(min_angle=20, full_angle=35, hold_target=3.0),
    "Right_Lateral_Flexion": ActionThreshold(min_angle=20, full_angle=35, hold_target=3.0),
    "Flexion":               ActionThreshold(min_angle=15, full_angle=30, hold_target=3.0),
    "Extension":             ActionThreshold(min_angle=10, full_angle=20, hold_target=3.0),
    "Left_Rotation":         ActionThreshold(min_angle=15, full_angle=35, hold_target=3.0),
    "Right_Rotation":        ActionThreshold(min_angle=15, full_angle=35, hold_target=3.0),
}
```

**关键点索引**（MediaPipe Pose 33 点）：
```
0  = 鼻尖 (nose)
7  = 右耳 (right ear)
8  = 左耳 (left ear)
11 = 左肩 (left shoulder)
12 = 右肩 (right shoulder)
```

### 1.7 FaceLandmarker 面部关键点检测器（0.2.0 新增）

**文件**：`detectors/face_landmarker/mediapipe.py`

**算法**：使用 MediaPipe `FaceLandmarker`（LIVE_STREAM 异步模式），检测面部的 **478 个关键点**（468 个面部 + 10 个虹膜）。

**输出接口**：
```python
def run(self, image: np.ndarray):           # 异步提交帧
def get_detect_result(self) -> dict | None:
    # 返回格式：
    {
        'face_landmarks': List[NormalizedLandmark],  # 第一张脸的 478 个关键点
    }
```

**配置**：通过 `config/face_detector.yaml` 配置（文件不存在时使用默认值，仅记录 warning）：
```yaml
model_path: "checkpoint/face_landmarker.task"
num_faces: 1
min_face_detection_confidence: 0.5
min_face_presence_confidence: 0.5
min_tracking_confidence: 0.5
```

**用途**：为面部空间坐标估计器提供精确的鼻尖、眼角等关键点（索引 1=鼻尖，33=左眼外角，263=右眼外角）。

### 1.8 面部空间坐标估计器（0.2.0 新增）

**文件**：`detectors/face_position_estimator/face_mesh_estimator.py`

**算法**：融合 FaceLandmarker + PoseLandmarker，估算用户面部相对摄像头的三维空间坐标。

**坐标系约定**：
- 原点：摄像头光心（Phase 1 用画面几何中心近似）
- X 轴：水平向右（+ 右 / - 左）
- Y 轴：垂直向上（+ 上 / - 下）
- Z 轴：沿光轴朝向用户（+ 朝向用户，正值）
- 单位：厘米 cm

**算法核心**：
- **X/Y**：用 Face Mesh 鼻尖归一化坐标减原点，除以瞳距归一化值，乘瞳距物理先验（6.3cm）。特性：用归一化坐标比值，与图像尺寸无关。
  ```
  X_cm = (nose_x_norm - cx_norm) / pupil_dist_norm * pupil_distance_cm
  Y_cm = -(nose_y_norm - cy_norm) / pupil_dist_norm * pupil_distance_cm  ← Y 取负号使 + 向上
  ```
- **Z**：用 Pose 的鼻尖 z 减双肩中点 z，乘头长物理先验（23cm）。语义：相对深度差（+ 低头 / - 后仰），不是绝对距离。
- **distance_cm**：用针孔模型 + FOV 假设估算绝对距离。
  ```
  distance_cm = pupil_distance_cm / (2 × tan(FOV/2) × pupil_dist_norm)
  ```

**输出接口**：
```python
def update(self, face_result, pose_result, image_w, image_h) -> FacePosition:
    # FacePosition dataclass 字段：
    # x_cm: float         # 水平偏移：+ 向右 / - 向左
    # y_cm: float         # 垂直偏移：+ 向上 / - 向下
    # z_cm: float         # 鼻尖相对双肩深度差：+ 低头 / - 后仰（不是绝对距离）
    # distance_cm: float  # 到摄像头绝对距离估算（针孔模型 + FOV 假设）
    # confidence: float   # 置信度 0~1
    # valid: bool         # 是否有效输出
```

**滤波与异常值剔除**：
- 一阶低通滤波 α=0.3（30% 新值 + 70% 历史值）
- 异常值剔除：单帧跳变 > 30cm 丢弃本帧（distance_cm 允许 2 倍阈值）
- 连续无效 10 帧重置滤波器

**配置文件** (`config/face_position.yaml`)：
```yaml
face_position:
  enabled: true
  pupil_distance_cm: 6.3        # 瞳距物理先验
  head_length_cm: 23.0         # 头长物理先验
  camera_fov_degrees: 60.0     # 摄像头 FOV（用于 distance_cm 估算）
  image_center_mode: "geometric"  # 原点模式：geometric / calibrated
  min_visibility: 0.5
  smoothing_alpha: 0.3
  outlier_jump_threshold_cm: 30.0
  invalid_reset_frames: 10
```

**精度说明**：
- X/Y 相对偏移：稳定，偏差 ±2-3cm（基于瞳距 6.3cm 先验）
- Z 相对深度：稳定，仅反映低头/后仰姿态
- distance_cm 绝对距离：依赖 FOV 假设，未标定精度 ±10~20%。Phase 2 标定后可用真实焦距替换。

### 1.9 MediaPipeAsyncProvider 通用基类（0.2.0 重构）

**文件**：`detectors/base/mediapipe_async_provider.py`

**设计动机**：原 GestureDetectorProvider / PoseDetectorProvider / FaceLandmarkerProvider 三个类的 `__init__`、`run()`、`close()`、`save_result` 回调模板几乎完全相同，存在严重代码重复。

**抽象基类设计**：

```python
class MediaPipeAsyncProvider(ABC):
    # 通用方法（子类无需重写）
    def _make_callback(self): ...     # 构造 LIVE_STREAM result_callback
    def run(self, image): ...         # mp.Image + _submit_async + try/except
    def close(self): ...              # 释放 landmarker 资源
    def __enter__/__exit__: ...       # 支持 with 语法

    # 抽象方法（子类必须实现）
    def _submit_async(self, mp_image, timestamp_ms): ...  # 指定异步方法名
    def _extract_result(self, result): ...                # 提取所需字段
    def get_detect_result(self) -> dict | None: ...        # 返回统一 dict 格式
```

**子类简化效果**：

| Provider | 改造前 | 改造后 |
|----------|--------|--------|
| GestureDetectorProvider | 86 行 | 84 行（含 _submit_async 指定 recognize_async）|
| PoseDetectorProvider | 71 行 | 67 行 |
| FaceLandmarkerProvider | 86 行 | 67 行 |

**关键 bug 修复**：重构过程中发现 GestureRecognizer 用 `recognize_async` 而非 `detect_async`，通过抽出 `_submit_async` 抽象方法解决了三个 detector 异步方法名不一致的问题。

### 1.10 统一 Detector 接口（0.2.0 重构）

所有 Detector base 类现在都实现统一接口：

| 方法 | EmotionDetectorProviderBase | MediaPipeAsyncProvider |
|------|-----------------------------|------------------------|
| detect(image) / run(image) | detect()（同步） | run()（异步） |
| get_detect_result() | - | ✓ |
| close() | ✓（默认空实现） | ✓（释放 landmarker） |
| __enter__/__exit__ | ✓ | ✓ |

**资源管理**：main.py 使用 `contextlib.ExitStack` 统一管理 detector 生命周期：

```python
with ExitStack() as stack:
    emotion_detector = stack.enter_context(EmotionDetectorProvider(...))
    gesture_detector = stack.enter_context(GestureDetectorProvider(...))
    pose_detector = stack.enter_context(PoseDetectorProvider(...))
    face_landmarker = stack.enter_context(FaceLandmarkerProvider(...))
    # ... 主循环 ...
# 退出 with 块时，所有 detector 的 close() 自动调用
```

---

## 2. 主程序流程

**文件**：`main.py`（0.2.0 重构后仅 200 行，原 322 行）

### 2.1 初始化流程

```
1. 读取 config/main.yaml 获取摄像头索引和日志级别
2. 使用 ExitStack 管理 detector 资源
3. 初始化情绪检测器（同步 API）
4. 初始化手势检测器（MediaPipe 异步）
5. 初始化肢体检测器（MediaPipe 异步）
6. 初始化颈部保健操检测器（无外部资源）
7. 初始化组合手势检测器
8. 初始化面部空间坐标估计器：
   - read_config_or_default("config/face_detector.yaml") 容错读取
   - FaceLandmarkerProvider + FacePositionEstimator
9. 打开摄像头
```

### 2.2 主循环流程

```
while True:
    1. 读取摄像头帧，水平翻转（镜像），转换为 RGB
    2. 情绪检测：emotion_detector.detect(rgb_frame) → {'emotion': '...'}
    3. 手势检测：gesture_detector.run(rgb_frame) → 异步
                gesture_detector.get_detect_result() → {'gesture': '...', 'hand_landmarks': [...]}
    4. 肢体检测：pose_detector.run(rgb_frame) → 异步
                pose_detector.get_detect_result() → {'pose_landmarks': [...]}
    5. 颈部保健操检测：neck_detector.update(pose_landmarks) → NeckAction
    6. 面部空间坐标：face_landmarker.run(rgb_frame) + face_pos_estimator.update() → FacePosition
    7. 组合手势检测：combo_detector.update(current_gesture) → combo_name 或 None
    8. 情绪联动检测：查表 EMOTION_GESTURE_COMBOS[(current_emotion, current_gesture)]
    9. 计算 FPS（一阶低通滤波，避免数值抖动）
    10. 绘制显示：
        - draw_overlay(): 双列信息覆盖层（左侧主信息 + 右侧辅助信息 + 左下角 FPS）
        - draw_hand_landmarks(): 手部关键点和连接线
        - draw_pose_landmarks(): 肢体关键点和骨骼连接线
    11. 按 'q' 退出
```

### 2.3 显示布局

**双列信息覆盖层**（`ui/overlay.py`）：

左侧主信息列 (x=10)：
- Emotion  (y=30)  - 情绪文本（颜色随情绪变化）
- Gesture  (y=60)  - 手势文本（颜色随手势变化）
- Combo    (y=90)  - 组合手势（颜色随组合变化）
- Pose     (y=120) - 肢体检测状态
- Neck     (y=150) - 颈部动作名称（颜色随动作变化）
- Face XYD (y=185) - 面部空间坐标 (x_cm, y_cm, distance_cm)

右侧辅助信息列 (x=screen_w - 280)：
- EmCombo   (y=30) - 情绪+手势联动
- NeckScore (y=55) - 颈部动作评分（仅动作非 None 时显示）
- Hold      (y=78) - 保持时间和角度（仅动作非 None 时显示）

左下角：
- FPS (y=screen_h - 10) - 帧率显示，颜色分级：绿(>25) / 黄(15-25) / 红(<15)

### 2.4 颜色映射

颜色定义集中在 `ui/colors.py`：

**情绪颜色 (BGR)**：
```python
EMOTION_COLORS = {
    "Happiness": (0, 255, 0),      # 绿色
    "Sadness": (255, 0, 0),        # 蓝色
    "Anger": (0, 0, 255),          # 红色
    "Fear": (0, 255, 255),         # 黄色
    "Surprise": (255, 0, 255),     # 紫色
    "Disgust": (0, 128, 128),      # 深黄
    "Contempt": (128, 128, 0),     # 深蓝绿
    "Neutral": (200, 200, 200),    # 灰色
}
```

**手势颜色 (BGR)**：
```python
GESTURE_COLORS = {
    "Closed_Fist": (0, 0, 255),    # 红色
    "Open_Palm": (0, 255, 0),      # 绿色
    "Pointing_Up": (255, 0, 0),    # 蓝色
    "Thumb_Down": (0, 128, 255),   # 橙色
    "Thumb_Up": (0, 255, 128),     # 青绿
    "Victory": (255, 255, 0),      # 青色
    "ILoveYou": (255, 0, 255),     # 紫色
}
```

**组合手势颜色 (BGR)**：
```python
COMBO_GESTURE_COLORS = {
    "Open": (0, 255, 0),           # 绿色
    "Close": (0, 0, 255),          # 红色
    "VolumeUp": (0, 200, 255),     # 橙色
    "VolumeDown": (255, 100, 0),   # 蓝色
}
```

**情绪联动颜色 (BGR)**：
```python
EMOTION_COMBO_COLORS = {
    "Welcome": (0, 255, 200),      # 青绿
    "Good": (0, 255, 128),         # 亮绿
}
```

**颈部动作颜色**（定义在 `detectors/pose_detector/neck_exercise.py`，与 ACTION_THRESHOLDS 配套）：
```python
NECK_ACTION_COLORS = {
    "Left_Lateral_Flexion":  (255, 180, 0),   # 蓝色
    "Right_Lateral_Flexion": (0, 180, 255),   # 橙色
    "Flexion":               (0, 255, 0),     # 绿色
    "Extension":             (0, 0, 255),     # 红色
    "Left_Rotation":         (255, 0, 180),   # 紫色
    "Right_Rotation":        (180, 0, 255),   # 粉色
    "None":                  (200, 200, 200), # 灰色
}
```

### 2.5 关键点绘制

关键点连接关系和绘制函数集中在 `ui/draw.py`：

**手部关键点连接关系**（21 个点）：
```python
HAND_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4),       # 拇指
    (0, 5), (5, 6), (6, 7), (7, 8),       # 食指
    (0, 9), (9, 10), (10, 11), (11, 12),  # 中指
    (0, 13), (13, 14), (14, 15), (15, 16),# 无名指
    (0, 17), (17, 18), (18, 19), (19, 20),# 小指
    (5, 9), (9, 13), (13, 17),            # 掌心横连
]
```

**肢体关键点连接关系**（33 个点，35 条连接）：
```python
POSE_CONNECTIONS = [
    # 面部（9 条）
    (0, 1), (1, 2), (2, 3), (3, 7),       # 右眼
    (0, 4), (4, 5), (5, 6), (6, 8),       # 左眼
    (9, 10),                               # 嘴
    # 躯干（4 条）
    (11, 12), (11, 23), (12, 24), (23, 24),
    # 右臂 + 左臂 + 右腿 + 左腿（22 条）
    ...
]
```

**绘制函数**：
- `draw_hand_landmarks(frame, hand_landmarks_list)`：黄色实心圆（半径 4px）+ 黑色边框 + 蓝色连接线（宽度 2px）
- `draw_pose_landmarks(frame, pose_landmarks, min_visibility=0.5)`：绿色实心圆（半径 3px）+ 黑色边框 + 橙色连接线（宽度 2px）

---

## 3. 已知问题与设计权衡

### 3.1 过度触发问题

纯事件驱动模型（无 hold_time、无 cooldown）在手势过渡期间可能因 MediaPipe 帧级抖动导致快速连触发。例如从握拳切换到伸掌时，可能在 1 秒内触发十几次 Open/Close。

**权衡**：用户选择了最快响应速度，接受过度触发。如需抑制抖动，可加回 `hold_time` 参数（建议 0.1s）。

### 3.2 MediaPipe 异步延迟

MediaPipe 使用 LIVE_STREAM 异步模式，`run()` 提交帧后结果通过回调返回，`get_detect_result()` 获取的是上一帧的处理结果，存在约 1 帧延迟。

### 3.3 情绪识别稳定性

MTCNN + emotiefflib 在低光照、侧脸、遮挡等场景下可能不稳定，情绪会在相邻类别间快速跳变。

### 3.4 面部空间坐标精度限制（0.2.0 新增）

- **distance_cm 依赖 FOV 假设**：未标定情况下精度 ±10~20%，FOV 偏差 ±5° 引起 distance 偏差约 ±10%
- **Z 轴是相对深度**：单目 Pose 无法给绝对距离，z_cm 仅反映低头/后仰姿态
- **Phase 2 改进方向**：摄像头标定 + PnP 求解，可显著提升绝对距离精度

---

## 4. 扩展指南

### 4.1 添加新的组合手势

编辑 `detectors/combo_gesture.py` 中的 `COMBO_GESTURES` 字典：

```python
COMBO_GESTURES = {
    # ... 已有组合
    ("Victory", "Open_Palm"): "Confirm",      # 新增：剪刀手→伸掌 = 确认
    ("Pointing_Up", "Closed_Fist"): "Previous", # 新增：指向上→握拳 = 上一个
}
```

同时在 `ui/colors.py` 中添加对应颜色：

```python
COMBO_GESTURE_COLORS = {
    # ... 已有颜色
    "Confirm": (255, 255, 255),    # 白色
    "Previous": (128, 0, 128),     # 紫色
}
```

### 4.2 添加新的情绪联动

编辑 `detectors/combo_gesture.py` 中的 `EMOTION_GESTURE_COMBOS` 字典：

```python
EMOTION_GESTURE_COMBOS = {
    # ... 已有联动
    ("Anger", "Closed_Fist"): "EmergencyStop",  # 新增：愤怒+握拳 = 紧急停止
}
```

同时在 `ui/colors.py` 中添加对应颜色：

```python
EMOTION_COMBO_COLORS = {
    # ... 已有颜色
    "EmergencyStop": (0, 0, 200),  # 深红
}
```

### 4.3 添加新的 MediaPipe 检测器（0.2.0 重构后）

继承 `MediaPipeAsyncProvider` 基类，仅需实现 3 个方法：

```python
from detectors.base.mediapipe_async_provider import MediaPipeAsyncProvider

class NewDetectorProvider(MediaPipeAsyncProvider):
    def __init__(self, model_path, ...):
        super().__init__()
        self.new_landmarks = None
        options = vision.NewLandmarkerOptions(
            base_options=python.BaseOptions(model_asset_path=model_path),
            running_mode=vision.RunningMode.LIVE_STREAM,
            # ... 各自参数
            result_callback=self._make_callback(),
        )
        self._landmarker = vision.NewLandmarker.create_from_options(options)

    def _submit_async(self, mp_image, timestamp_ms):
        # 指定具体的异步方法名（detect_async / recognize_async）
        self._landmarker.detect_async(mp_image, timestamp_ms)

    def _extract_result(self, result):
        # 从 result 提取所需字段
        self.new_landmarks = None
        if result.new_landmarks:
            self.new_landmarks = result.new_landmarks[0]

    def get_detect_result(self) -> dict | None:
        if self.new_landmarks is None:
            return None
        return {'new_landmarks': self.new_landmarks}
```

### 4.4 配置文件容错读取（0.2.0 新增）

`utils/config.py` 提供两种读取方式：

```python
# 严格模式（必需配置，失败抛异常）
config = read_config("config/main.yaml")

# 容错模式（可选配置，失败返回默认值 + warning）
config = read_config_or_default("config/optional.yaml", default={})
```

---

## 5. 架构演进记录（0.2.0）

### 5.1 0.1.0 → 0.2.0 重构摘要

| 维度 | 0.1.0 | 0.2.0 | 改动 |
|------|-------|-------|------|
| Detector 数量 | 4 | 7 | +颈部 / FaceLandmarker / FacePosition |
| 代码重复 | 3 个 Provider 模板复制 | MediaPipeAsyncProvider 基类 | -150 行重复 |
| main.py 行数 | 322 | 200 | -38%（拆到 ui/ 模块） |
| 资源管理 | 手动 close | ExitStack + with | 自动释放 |
| 接口统一 | 部分不一致 | close + __enter__/__exit__ | 统一 |
| 配置容错 | 抛异常 | read_config_or_default | 降级 |
| 性能监控 | 无 | FPS 显示 + 颜色分级 | 可视化 |

### 5.2 模块依赖关系

```
main.py
├── detectors/
│   ├── base/
│   │   └── mediapipe_async_provider.py  ← 通用基类（0.2.0 新增）
│   ├── emotion_detector/
│   │   ├── base.py
│   │   └── emotiefflib.py
│   ├── gesture_detector/
│   │   ├── base.py ← 继承 MediaPipeAsyncProvider
│   │   └── mediapipe.py ← ~80 行（原 86 行）
│   ├── pose_detector/
│   │   ├── base.py ← 继承 MediaPipeAsyncProvider
│   │   ├── mediapipe.py ← ~65 行（原 71 行）
│   │   └── neck_exercise.py ← 颈部保健操（0.2.0 新增）
│   ├── face_landmarker/
│   │   ├── base.py ← 继承 MediaPipeAsyncProvider
│   │   └── mediapipe.py ← ~65 行（原 86 行）
│   ├── face_position_estimator/
│   │   ├── base.py ← FacePosition dataclass（0.2.0 新增）
│   │   └── face_mesh_estimator.py ← 主算法（0.2.0 新增）
│   └── combo_gesture.py
├── ui/
│   ├── colors.py ← 颜色映射（0.2.0 新增，从 main.py 拆出）
│   ├── draw.py ← 关键点绘制（0.2.0 新增，从 main.py 拆出）
│   └── overlay.py ← 双列显示覆盖层（0.2.0 新增）
└── utils/
    ├── config.py ← 新增 read_config_or_default
    └── logger.py
```

### 5.3 依赖

**Python 包**（详见 `pyproject.toml`）：
- `emotiefflib>=1.0` - 情绪识别
- `facenet-pytorch>=2.6.0` - MTCNN 人脸检测
- `loguru>=0.7.3` - 日志
- `mediapipe>=0.10.21` - 手势/肢体/面部检测
- `opencv-python>=4.0.0` - 图像处理
- `pyyaml>=6.0` - 配置文件

**MediaPipe 模型文件**（位于 `checkpoint/`）：
- `gesture_recognizer.task` - 手势识别
- `pose_landmarker_full.task` - 肢体检测
- `face_landmarker.task` - 面部关键点（478 点，3.76MB）

---

## 6. 测试与验证

### 6.1 端到端测试

启动主程序：
```bash
python main.py
```

预期输出：
```
- INFO - Emotion detector initialized.
- INFO - Gesture detector initialized.
- INFO - Pose detector initialized.
- INFO - Neck exercise detector initialized.
- INFO - Face position estimator initialized.
- INFO - Camera opened. Press 'q' to quit.
```

画面显示：
- 左侧主信息列：Emotion / Gesture / Combo / Pose / Neck / Face XYD
- 右侧辅助信息列：EmCombo / NeckScore / Hold
- 左下角：FPS（颜色随帧率变化）
- 手部关键点（黄色 + 蓝色连接线）
- 肢体关键点（绿色 + 橙色连接线）

按 `q` 退出，资源由 ExitStack 自动释放。

### 6.2 独立模块测试

- `test_face_landmarker.py` - FaceLandmarker 478 关键点输出验证
- `test_face_position_estimator.py` - FacePosition X/Y/Z/Dist 输出验证
