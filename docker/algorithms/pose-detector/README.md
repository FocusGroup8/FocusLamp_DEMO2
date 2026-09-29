# pose-detector

> DEMO2 算法微服务 | 端口 9010 | 容器名 pose-detector

## 功能
人体姿态检测 + 颈部运动检测服务。使用 MediaPipe PoseLandmarker（IMAGE 同步模式，num_poses=1）检测人脸/躯干姿态关键点（33 点）；并内置 NeckExerciseDetector（颈部运动检测，权威实现来自 FocusDetector）判定左侧屈、右侧屈、前屈、后伸、左旋转、右旋转等动作。

输出 `pose_detected`、`pose_landmarks`（33 点 `[x, y, z, visibility]`）与 `neck_action`（`name`、`angle`、`amplitude_score`、`hold_score`、`total_score`、`hold_duration`）。

## 目录结构
- `server.py`：Flask 服务入口，包含 PoseLandmarker 加载、姿态检测、颈部动作检测器（NeckExerciseDetector / NeckAction / ActionThreshold）、可视化（姿态骨骼 + 中文颈部动作信息）。
- 模型文件：`pose_landmarker_full.task`（MediaPipe PoseLandmarker full 模型）。
- `Dockerfile`（安装中文字体 `fonts-wqy-microhei`）、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==5.0.0.93
  - numpy==2.0.2
  - mediapipe==0.10.14
  - pillow==11.3.0
  - gunicorn==23.0.0
- 模型文件：`pose_landmarker_full.task`（MediaPipe Task 格式，与 server.py 同目录）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d pose-detector`（compose 内部注入 `PORT=9010`）。
- 直接运行：在 `d:\Docker\docker\algorithms\pose-detector` 下执行 `python server.py`，可用环境变量指定端口，例如 `PORT=9010 python server.py`（默认 PORT=9010）。
- 接口（默认端口 9010）：
  - `GET /health`：健康检查，返回 `{"status": "ok"}`。
  - `POST /reset`：重置颈部运动检测器状态。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。
- 并发说明：MediaPipe PoseLandmarker 不保证线程安全，`detect()` 调用通过 `pose_lock` 串行化。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理，未要求 GPU。