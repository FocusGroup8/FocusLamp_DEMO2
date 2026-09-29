# gesture-mantis-detector

> DEMO2 算法微服务 | 端口 9009 | 容器名 gesture-mantis-detector

## 功能
手势识别服务（MediaPipe GestureRecognizer，IMAGE 同步模式，num_hands=1）。识别单手手势并支持组合手势判定：

- 手势组合：`Closed_Fist` + `Open_Palm` → `Open`；`Open_Palm` + `Closed_Fist` → `Close`；`Closed_Fist` + `Thumb_Up` → `VolumeUp`；`Closed_Fist` + `Thumb_Down` → `VolumeDown`。
- 情绪+手势组合（需请求携带 `current_emotion`）：`Happiness` + `Open_Palm` → `Welcome`；`Happiness` + `Thumb_Up` → `Good`。

输出 `gesture`、`confidence`、`hand_detected`、`hand_landmarks_count`、`hand_landmarks`、`combo_gesture`。

## 目录结构
- `server.py`：Flask 服务入口，包含 GestureRecognizer 加载、单手手势识别、组合手势状态机（ComboGestureDetector）、可视化。
- 模型文件：`gesture_recognizer.task`（MediaPipe GestureRecognizer 模型）。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - mediapipe==0.10.14
  - gunicorn==23.0.0
- 模型文件：`gesture_recognizer.task`（MediaPipe Task 格式）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d gesture-mantis-detector`（compose 内部注入 `PORT=9009`）。
- 直接运行：在 `d:\Docker\docker\algorithms\gesture-mantis` 下执行 `python server.py`，可用环境变量指定端口，例如 `PORT=9009 python server.py`（默认 PORT=9009）。
- 接口（默认端口 9009）：
  - `GET /health`：健康检查，返回上次手势、是否检出人手等状态。
  - `POST /reset`：重置手势状态与组合手势状态机。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像；可携带 `current_emotion` 以支持情绪+手势组合。
  - 请求体：`{"image": "<base64>"}`（`/detect_visualize` 可附加 `"current_emotion": "<情绪>"`）。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理，未要求 GPU。