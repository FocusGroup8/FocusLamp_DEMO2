# gesture-detector

> DEMO2 算法微服务 | 端口 9002 | 容器名 —（遗留模块，未在 docker-compose.yml 中启用）

## 功能
手势关键点检测服务（**遗留实现**）。使用 ONNX 手部姿态模型 `resnet_50_size-256.onnx` 输出 21 个手部关键点，并用 MediaPipe HandLandmarker 关键点进行绘制；含关键点平滑（`SMOOTHING_ALPHA=0.3`）与历史队列（`KEYPOINT_HISTORY_SIZE=5`）以稳定输出。

注意：该模块在 DEMO2 中已被 `gesture-mantis`（MediaPipe GestureRecognizer）取代，此处作为遗留/参考实现保留，**未在 docker-compose.yml 中启用**。

## 目录结构
- `server.py`：Flask 服务入口，包含 ONNX 手部姿态模型加载、21 关键点推理与平滑、MediaPipe HandLandmarker 绘制、可视化。
- `download_model.py`：构建时下载 `hand_landmarker.task`（来自 MediaPipe 官方模型库）。
- 模型文件：`resnet_50_size-256.onnx`（ONNX 手部姿态模型）。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - onnxruntime==1.19.2
  - mediapipe==0.10.35
  - gunicorn==23.0.0
- 模型文件：
  - `resnet_50_size-256.onnx`（ONNX，输入 256×256）。
  - `hand_landmarker.task`（MediaPipe HandLandmarker，构建时由 `download_model.py` 下载）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：**该模块未纳入 docker-compose.yml**，无法通过 `docker-compose up -d` 直接启动；如有需要可单独 `docker build` 后运行。
- 直接运行：在 `d:\Docker\docker\algorithms\gesture` 下先执行 `python download_model.py` 下载手部模型，再执行 `python server.py`。注意：`server.py` 中默认端口为 `8002`（`os.environ.get('PORT', 8002)`），例如 `PORT=9002 python server.py`。
- 接口（默认端口 8002，任务约定端口 9002）：
  - `GET /health`：健康检查，返回算法名与版本（version 3.0.0，method handpose_x_ONNX）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理；若存在 CUDA provider 会自动优先使用 GPU。