# FocusDetector（单机版专注检测器）

> DEMO2 | 非容器单机程序（桌面 GUI，无固定端口）

## 功能

基于本地摄像头的实时专注/行为检测演示程序（`pyproject.toml`：name `FocusDetector`，version `0.2.0`，`requires-python >=3.10`）。聚合 6 类检测能力并在同一窗口内实时叠加显示：

1. **情绪检测**（EmotiEffLib）：输出情绪类别与颜色。
2. **手势检测**（MediaPipe GestureRecognizer）：输出单手手势。
3. **组合手势**（`detectors/combo_gesture.py`）：将单手势序列聚合为组合手势。
4. **姿态与颈部运动**（`pose_detector` + `neck_exercise`）：输出人体姿态关键点与颈部保健操动作。
5. **人脸网格**（`face_landmarker`）：输出人脸关键点。
6. **面部空间坐标**（`face_position_estimator`）：结合人脸与姿态结果估计面部空间坐标。

结果通过 UI 叠加绘制模块（`ui/overlay.py`、`ui/draw.py`、`ui/colors.py`）在画面上显示，包含情绪/手势文本、组合手势、颈部动作、面部坐标与实时 FPS。按 `q` 键退出，`Ctrl+C` 或退出时由 `ExitStack` 统一释放检测器资源。

## 目录结构

```
FocusDetector/
├── main.py                      # 入口：采集、调度各检测器、绘制与显示
├── detectors/
│   ├── emotion_detector/        # 情绪检测（EmotiEffLib）
│   ├── gesture_detector/        # 手势检测（MediaPipe）
│   ├── pose_detector/           # 姿态检测 + neck_exercise（颈部运动）
│   ├── face_landmarker/         # 人脸网格（MediaPipe）
│   ├── face_position_estimator/ # 面部空间坐标估计
│   ├── base/                    # MediaPipe 异步 Provider 基类
│   └── combo_gesture.py         # 组合手势检测
├── ui/
│   ├── overlay.py               # 叠加信息绘制
│   ├── draw.py                  # 关键点绘制
│   └── colors.py                # 颜色映射
├── utils/
│   ├── config.py                # 读取 YAML 配置
│   └── logger.py                # 日志初始化（loguru）
├── config/
│   ├── main.yaml                # camera_index、log_level
│   ├── emotion_detector.yaml
│   ├── gesture_detector.yaml
│   ├── pose_detector.yaml
│   └── face_position.yaml
├── checkpoint/                  # 模型文件
│   ├── ssd_mobilenet_v2.tflite
│   ├── pose_landmarker_full.task
│   ├── hand_landmarker.task
│   ├── gesture_recognizer.task
│   └── face_landmarker.task
├── docs/                        # 设计文档
├── REPORT.md
├── test_*.py                    # 各检测能力的独立测试脚本
└── yolo26s.pt                   # YOLO 权重（供 phone/game 相关测试脚本使用）
```

## 依赖

`pyproject.toml` 声明：

```
emotiefflib>=1.0
facenet-pytorch>=2.6.0
loguru>=0.7.3
mediapipe>=0.10.21
numpy>=1.24
opencv-python>=4.0.0
pyyaml>=6.0
```

另含 `yolo26s.pt`（YOLO 权重，供 phone/game 相关测试脚本使用）。

## 运行方式

在本目录（`d:\Docker\algorithm\FocusDetector`）下执行：

```bash
python main.py
```

- 读取 `config/main.yaml` 的 `camera_index`（默认 0）与 `log_level`（默认 DEBUG）。
- 弹窗显示实时检测画面，按 `q` 退出。

也可安装为命令行脚本：

```bash
pip install -e .
focus-detector
```

## 对应硬件

- 需要**本地 USB 摄像头**（`config/main.yaml` 的 `camera_index` 可配置）。
- **无需 ESP32**：这是单机演示程序，不依赖 Docker 或 ESP32-P4 灯头板。

## 对外接口或协议

无网络接口。本程序为本地桌面演示程序，不监听端口，也不对外提供 HTTP/WebSocket 服务；与 DEMO2 容器体系中的 focus 算法服务（`docker/algorithms/focus`，端口 9003）为**相互独立的实现**。