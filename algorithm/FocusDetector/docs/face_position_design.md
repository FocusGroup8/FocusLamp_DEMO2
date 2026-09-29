# 面部空间坐标估计 - 设计文档

> 生成日期：2026-06-27 | 版本：0.1.0 | 状态：设计草案

---

## 1. 目标

基于笔记本内置单目摄像头，实时估计用户面部相对摄像头的三维空间坐标（单位：cm），输出稳定的 (X, Y, Z) 物理坐标供 FocusDetector 后续功能（专注度监测、姿态判断、空间交互）使用。

### 1.1 关键约束

| 维度 | 约束 |
|------|------|
| 用途 | 精确空间定位 |
| 硬件 | 仅笔记本内置单目摄像头（不可加硬件） |
| 精度 | 稳定输出，绝对偏差 ±5~10cm 可接受 |
| 延迟 | < 200ms |
| 单位 | cm |
| 坐标系 | OpenCV 约定：X 右、Y 下、Z 朝向用户 |
| 原点 | 摄像头光心；Phase 1 用画面几何中心 (W/2, H/2) 近似，Phase 2 标定后替换为真实 (cx, cy) |

### 1.2 核心矛盾

单目摄像头存在尺度歧义（scale ambiguity）：同一画面既可解释为近处小脸、也可解释为远处大脸。必须引入一个"已知物理尺寸"作为先验来解算尺度。

**选用先验**：瞳距 6.3cm（成年人均值，个体差异 ±0.5cm）。

---

## 2. 总体架构

### 2.1 模块结构

```
FocusDetector/
├── detectors/
│   └── face_position_estimator/            # 新增模块
│       ├── __init__.py
│       ├── base.py                         # FacePosition dataclass + 基类
│       └── face_mesh_estimator.py          # Face Mesh + Pose 融合实现
├── config/
│   └── face_position.yaml                  # 新增配置
├── checkpoint/
│   └── face_landmarker.task                # 新增：Face Mesh 模型（需下载）
└── main.py                                 # 集成显示 X/Y/Z
```

### 2.2 数据流

```
RGB frame
   │
   ├─→ FaceLandmarker.run() ──→ 468 landmarks (x, y, z)
   │                              │
   │                              ▼
   │                        提取关键点：
   │                          鼻尖 (landmark 1)
   │                          左眼外角 (landmark 33)
   │                          右眼外角 (landmark 263)
   │                        瞳距像素 = dist(33, 263) * W
   │                        scale = 6.3 / 瞳距像素  (cm/pixel)
   │                        X_cm = (鼻尖x*W - cx) * scale
   │                        Y_cm = (鼻尖y*H - cy) * scale
   │
   ├─→ PoseLandmarker.run() ──→ 33 landmarks (x, y, z, vis)
   │                              │
   │                              ▼
   │                        提取关键点：
   │                          鼻尖 (idx 0)
   │                          左肩 (idx 11)
   │                          右肩 (idx 12)
   │                        可见度 ≥ 0.5 检查
   │                        z_diff = 鼻尖z - 双肩中点z
   │                        Z_cm = z_diff * HEAD_LENGTH_CM (23cm 先验)
   │
   ▼
合并 → FacePosition(X, Y, Z, confidence, valid)
   │
   ▼
异常值剔除（跳变 > 30cm 丢弃，不更新滤波器）
   │
   ▼
一阶低通滤波 α=0.3
   │
   ▼
输出 FacePosition
```

### 2.3 坐标系定义

```
        Z (朝向用户，+)
         ↑
         │
         │
         o──────→ X (向右，+)
         │
         │
         ↓
         Y (向下，+)

原点 o = 摄像头光心（Phase 1 用画面几何中心近似）
```

---

## 3. 详细设计

### 3.1 数据类

```python
# detectors/face_position_estimator/base.py
from dataclasses import dataclass

@dataclass
class FacePosition:
    x_cm: float          # 水平偏移：+ 右 / - 左
    y_cm: float          # 垂直偏移：+ 下 / - 上
    z_cm: float          # 沿光轴距离：+ 朝向用户
    confidence: float    # 0~1，基于关键点可见度
    valid: bool          # 是否有效输出
```

### 3.2 配置文件

```yaml
# config/face_position.yaml
face_position:
  enabled: true

  # 尺度先验
  pupil_distance_cm: 6.3          # 瞳距物理先验（cm）
  head_length_cm: 23.0           # 头长物理先验（cm），用于 Z 轴换算

  # 原点配置
  image_center_mode: "geometric" # "geometric" = (W/2, H/2)；"calibrated" = 用下方 cx/cy
  calibrated_cx: null            # Phase 2 标定后填入
  calibrated_cy: null

  # 关键点可见度
  min_visibility: 0.5

  # 滤波与异常值
  smoothing_alpha: 0.3            # 一阶低通系数：0=冻结，1=不滤波
  outlier_jump_threshold_cm: 30.0
  invalid_reset_frames: 10       # 连续无效帧数后重置滤波器

  # 输出
  output_unit: "cm"
```

### 3.3 Face Mesh Estimator 实现要点

```python
# detectors/face_position_estimator/face_mesh_estimator.py

class FacePositionEstimator:
    """Face Mesh + Pose 融合的面部空间坐标估计器"""

    # MediaPipe Face Mesh 关键点索引
    NOSE_TIP = 1                  # 鼻尖
    LEFT_EYE_OUTER = 33           # 左眼外角
    RIGHT_EYE_OUTER = 263         # 右眼外角

    # MediaPipe Pose 关键点索引
    POSE_NOSE = 0
    POSE_LEFT_SHOULDER = 11
    POSE_RIGHT_SHOULDER = 12

    def __init__(self, face_landmarker, pose_landmarker, config: dict):
        self.face_landmarker = face_landmarker
        self.pose_landmarker = pose_landmarker
        self.cfg = config

        # 滤波状态
        self._last_valid_pos: FacePosition | None = None
        self._invalid_count = 0

    def update(self, image_w: int, image_h: int) -> FacePosition:
        face_result = self.face_landmarker.get_result()
        pose_result = self.pose_landmarker.get_result()

        # 1. 检查 Face Mesh 是否有效
        if not face_result or not face_result.face_landmarks:
            return self._handle_invalid()
        face_lms = face_result.face_landmarks[0]

        # 2. 检查 Pose 是否有效
        if not pose_result or not pose_result.pose_landmarks:
            return self._handle_invalid()
        pose_lms = pose_result.pose_landmarks[0]

        # 3. 可见度检查（Pose 关键点）
        nose_vis = pose_lms[POSE_NOSE].visibility
        l_shoulder_vis = pose_lms[POSE_LEFT_SHOULDER].visibility
        r_shoulder_vis = pose_lms[POSE_RIGHT_SHOULDER].visibility
        if min(nose_vis, l_shoulder_vis, r_shoulder_vis) < self.cfg['min_visibility']:
            return self._handle_invalid()

        # 4. 计算瞳距像素 → 尺度因子
        left_eye = face_lms[LEFT_EYE_OUTER]
        right_eye = face_lms[RIGHT_EYE_OUTER]
        pupil_dist_px = abs(right_eye.x - left_eye.x) * image_w
        if pupil_dist_px < 5:  # 异常过小
            return self._handle_invalid()
        scale = self.cfg['pupil_distance_cm'] / pupil_dist_px  # cm/pixel

        # 5. X/Y 坐标（基于鼻尖像素位置）
        cx, cy = self._get_center(image_w, image_h)
        nose_x_px = face_lms[NOSE_TIP].x * image_w
        nose_y_px = face_lms[NOSE_TIP].y * image_h
        x_cm = (nose_x_px - cx) * scale
        y_cm = (nose_y_px - cy) * scale

        # 6. Z 坐标（Pose z 差值 × 头长先验）
        nose_z = pose_lms[POSE_NOSE].z
        mid_shoulder_z = (pose_lms[POSE_LEFT_SHOULDER].z + pose_lms[POSE_RIGHT_SHOULDER].z) / 2
        z_diff = nose_z - mid_shoulder_z  # 鼻尖相对双肩中点的深度差
        z_cm = z_diff * self.cfg['head_length_cm']

        # 7. 置信度
        confidence = (nose_vis + l_shoulder_vis + r_shoulder_vis) / 3

        # 8. 异常值剔除
        new_pos = FacePosition(x_cm, y_cm, z_cm, confidence, valid=True)
        if self._is_outlier(new_pos):
            return self._last_valid_pos or FacePosition(0, 0, 0, 0, valid=False)

        # 9. 一阶低通滤波
        if self._last_valid_pos is not None:
            alpha = self.cfg['smoothing_alpha']
            new_pos.x_cm = alpha * new_pos.x_cm + (1 - alpha) * self._last_valid_pos.x_cm
            new_pos.y_cm = alpha * new_pos.y_cm + (1 - alpha) * self._last_valid_pos.y_cm
            new_pos.z_cm = alpha * new_pos.z_cm + (1 - alpha) * self._last_valid_pos.z_cm

        self._last_valid_pos = new_pos
        self._invalid_count = 0
        return new_pos

    def _get_center(self, w, h):
        mode = self.cfg['image_center_mode']
        if mode == "calibrated" and self.cfg.get('calibrated_cx') is not None:
            return self.cfg['calibrated_cx'], self.cfg['calibrated_cy']
        return w / 2, h / 2

    def _is_outlier(self, new_pos: FacePosition) -> bool:
        if self._last_valid_pos is None:
            return False
        threshold = self.cfg['outlier_jump_threshold_cm']
        dx = abs(new_pos.x_cm - self._last_valid_pos.x_cm)
        dy = abs(new_pos.y_cm - self._last_valid_pos.y_cm)
        dz = abs(new_pos.z_cm - self._last_valid_pos.z_cm)
        return dx > threshold or dy > threshold or dz > threshold

    def _handle_invalid(self) -> FacePosition:
        self._invalid_count += 1
        if self._invalid_count >= self.cfg['invalid_reset_frames']:
            self._last_valid_pos = None
            self._invalid_count = 0
        if self._last_valid_pos is not None:
            # 保持上一帧输出，但标记为低置信度
            pos = FacePosition(
                self._last_valid_pos.x_cm,
                self._last_valid_pos.y_cm,
                self._last_valid_pos.z_cm,
                confidence=0.0,
                valid=False,
            )
            return pos
        return FacePosition(0, 0, 0, 0, valid=False)
```

### 3.4 main.py 集成

在 [main.py](file:///d:/Internship/new/algorithm/FocusDetector/main.py) 主循环追加：

```python
from detectors.face_position_estimator.face_mesh_estimator import FacePositionEstimator
from detectors.face_landmarker.mediapipe import FaceLandmarkerProvider

# 初始化
face_config = read_config("config/face_position.yaml")
face_landmarker = FaceLandmarkerProvider(
    model_path="checkpoint/face_landmarker.task",
    min_face_detection_confidence=0.5,
    min_face_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
face_pos_estimator = FacePositionEstimator(face_landmarker, pose_detector, face_config)

# 主循环内（在 pose_detector.run 之后）：
face_landmarker.run(rgb_frame)
face_pos = face_pos_estimator.update(frame.shape[1], frame.shape[0])
if face_pos.valid:
    color = (0, 255, 255)
    text = f"Face XYZ: ({face_pos.x_cm:+.1f}, {face_pos.y_cm:+.1f}, {face_pos.z_cm:+.1f}) cm"
else:
    color = (100, 100, 100)
    text = "Face XYZ: -- (invalid)"
cv2.putText(display_frame, text, (10, 320),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
```

---

## 4. 错误处理

| 场景 | 处理 |
|------|------|
| Face Mesh 未检测到脸 | `valid=False`，保持上一帧输出 |
| Pose 关键点可见度 < 0.5 | `valid=False`，保持上一帧输出 |
| 瞳距像素 < 5（异常小，用户过远或侧脸） | `valid=False` |
| 单帧坐标跳变 > 30cm | 判为异常值丢弃，不更新滤波器 |
| 连续 10 帧无效 | 重置滤波器状态，输出 `valid=False` |
| Pose 双肩归一化距离 < 0.05 | 视为用户过远，`valid=False` |

---

## 5. 验证方法

### 5.1 静态精度测试

| 真实距离 | 期望输出 Z | 允许偏差 |
|---------|-----------|---------|
| 30cm | ~30 | ±5cm |
| 50cm | ~50 | ±5cm |
| 80cm | ~80 | ±10cm |
| 100cm | ~100 | ±10cm |

测试方法：用户用尺子测量面部到摄像头的距离，对比程序输出 Z 值。

### 5.2 动态稳定性测试

- 左右移动：观察 X 坐标平滑性，无明显抖动
- 前后移动：观察 Z 坐标平滑性，无明显滞后
- 自然头部摆动：异常值剔除生效，无突变

### 5.3 边界情况测试

- 侧脸 45°：Face Mesh 仍能检测时，验证输出是否合理
- 遮挡半张脸：触发 `valid=False`，保持上一帧输出
- 快速移动出画面再回来：连续无效 10 帧后重置

---

## 6. 已知限制

### 6.1 Z 轴精度受限

MediaPipe Pose 的 z 是相对臀部中点归一化的相对深度，单位近似为"头长"。用头长 23cm 先验做线性映射，绝对精度有限（±5~10cm）。受个体头长差异、姿态变化影响。

### 6.2 大角度侧脸退化

Face Mesh 在侧脸超过 45° 时检测精度显著下降，瞳距像素测量误差增大。

### 6.3 原点近似偏差

Phase 1 用画面几何中心 (W/2, H/2) 作为光心近似，与真实光心 (cx, cy) 存在像素级偏差（通常 <10px），对应物理偏差 <1cm，可接受。

### 6.4 瞳距个体差异

瞳距 6.3cm 是均值，实际个体差异 ±0.5cm（成人范围 5.8~6.8cm），对应尺度误差约 ±8%。可后续支持用户校准。

---

## 7. 未来扩展（Phase 2，本次不实现）

| 升级项 | 收益 | 代价 |
|--------|------|------|
| 摄像头内参标定（棋盘格） | (cx, cy) 替代画面中心；焦距 f 用于 Z 针孔反推 | 一次性标定流程 |
| cv2.solvePnP + 3D 面部模板 | 6DoF 位姿，Z 精度 ±2~5cm | 增加模板数据 |
| 瞳距用户校准 | 消除个体差异 ±8% 误差 | UI 校准流程 |
| 卡尔曼滤波替代一阶低通 | 更平滑、更跟手 | 实现复杂度增加 |
| 多帧融合 + RANSAC | 抵抗异常帧 | 计算开销 |

---

## 8. 依赖与资源

### 8.1 新增依赖

- MediaPipe Face Landmarker 模型：`checkpoint/face_landmarker.task`
  - 下载地址：https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task
- 模块代码：约 200 行（base.py + face_mesh_estimator.py）

### 8.2 复用已有

- `detectors/pose_detector/mediapipe.py`：PoseDetectorProvider 直接复用
- `detectors/pose_detector/base.py`：基类模式
- `utils/logger.py`、`utils/config.py`：基础设施
- `config/pose_detector.yaml`：Pose 配置

### 8.3 摄像头标定流程（Phase 2 备用）

```bash
# 1. 打印棋盘格（9x6 内角点，方格 25mm）
# 2. 拍摄 10~20 张不同角度照片
# 3. 运行标定脚本
python utils/camera_calibration.py --images ./calib_images/ --output config/camera_intrinsics.yaml
# 4. 把 cx, cy, fx, fy 写入 face_position.yaml
```
