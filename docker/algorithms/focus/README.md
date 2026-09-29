# focus-detector

> DEMO2 算法微服务 | 端口 9003 | 容器名 focus-detector

## 功能
专注度/参与度检测综合服务。接收 base64 图像帧，加载 EngageDetectorProvider（Mantis，ONNX engage 模型）判断参与度等级，并通过 PomodoroManager（番茄钟，WINDOW_DURATION 默认 90 秒）按窗口累计参与度统计，输出专注度等级与得分。

同时挂载三个 MediaPipe 辅助检测器：EyeDetector（眼睛）、FaceDetector（人脸）、GestureDetector（手势），并把各自结果一并返回。

输入：`{"image": "<base64>"}`；输出：`engage_detect_result`（engage_level / engage_level_name）、`focus_level` / `focus_level_name` / `focus_score` / `engage_level_count`，以及可选的 `eye_detect_result`、`face_detect_result`、`gesture_detect_result`。

## 目录结构
- `server.py`：Flask 服务入口，负责模型加载、帧处理、结果汇总与可视化。
- `core/providers/engage_detector/`：参与度检测（`base.py` 定义接口，`mantis.py` 为 ONNX engage 模型实现，即 EngageDetectorProvider）。
- `core/providers/eye_detector/`：眼睛检测（MediaPipe）。
- `core/providers/face_detector/`：人脸检测（MediaPipe）。
- `core/providers/gesture_detector/`：手势检测（MediaPipe）。
- `core/manager/pomodoro_manager.py`：番茄钟窗口管理，累计参与度并计算专注度得分/等级。
- `core/types/`：`engage.py`（参与度等级定义）、`focus_corr_state.py`（专注度等级与权重）。
- `core/utils/`：`settings.py`、`logger.py`（配置与日志工具）。
- `config/`：`engage_detector.yaml`、`eye_detector.yaml`、`face_detector.yaml`、`gesture_detector.yaml`。
- 模型文件：`engage_model.onnx`、`face_landmarker.task`、`gesture_recognizer.task`、`blaze_face_short_range.tflite`。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.11.0.86
  - numpy==1.26.4
  - mediapipe==0.10.14
  - onnxruntime==1.19.2
  - torch==2.5.1+cpu
  - pyyaml==6.0.3
  - gunicorn==23.0.0
- 模型文件：
  - `engage_model.onnx`（ONNX，Mantis 参与度模型）
  - `face_landmarker.task`（MediaPipe，含虹膜关键点）
  - `gesture_recognizer.task`（MediaPipe 手势识别）
  - `blaze_face_short_range.tflite`（MediaPipe 人脸检测，短距）
  - `config/*.yaml`：各检测器的模型路径与阈值配置。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d focus-detector`（compose 内部会注入 `PORT=9003` 与 `WINDOW_DURATION=90`）。
- 直接运行：在 `d:\Docker\docker\algorithms\focus` 下执行 `python server.py`，可通过环境变量指定端口与窗口时长，例如 `PORT=9003 WINDOW_DURATION=90 python server.py`（默认 PORT=9003）。
- 接口（默认端口 9003）：
  - `GET /health`：健康检查，返回累计帧数、是否就绪、当前专注度等级及辅助检测器加载状态。
  - `POST /reset`：重置参与度队列、推理结果与番茄钟状态。
  - `POST /detect`：返回检测结果（不返回可视化图像）。
  - `POST /detect_visualize`：返回检测结果与可视化图像；内部使用 `processing_lock`，忙碌时直接返回 503（`Server busy, skipping frame`），main-client 侧超时为 8s。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像（来源于 ESP32P4 摄像头流，经 main-client 转发）。为纯 CPU 推理，未要求 GPU。