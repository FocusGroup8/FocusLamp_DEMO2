# head-pose-detector

> DEMO2 算法微服务 | 端口 9001 | 容器名 —（遗留模块，未在 docker-compose.yml 中启用）

## 功能
头部姿态估计服务（**遗留实现**）。使用 WHENet（`WHENet.h5`）估计头部姿态角 yaw / pitch / roll，并使用 MediaPipe FaceDetection 做人脸检测；当 WHENet 加载失败时，自动回退到 MediaPipe FaceMesh + `cv2.solvePnP` 的方案。

输出 `face_x`、`face_y`、`yaw`、`pitch`、`roll` 与 `method`（`WHENet` 或 `MediaPipe`）。

注意：该模块在 DEMO2 中**未在 docker-compose.yml 中启用**，作为遗留/参考实现保留。

## 目录结构
- `server.py`：Flask 服务入口，包含 WHENet 加载与回退逻辑、MediaPipe 人脸检测/网格、PnP 姿态求解与可视化。
- `whenet.py`：WHENet 模型封装。
- `utils.py`：辅助工具函数。
- 模型文件：`WHENet.h5`（Keras/H5 格式头部姿态模型）。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - scipy==1.13.1
  - mediapipe==0.10.14
  - tensorflow-cpu==2.19.1
  - efficientnet==1.1.1
  - keras==3.10.0
  - gunicorn==23.0.0
- 模型文件：`WHENet.h5`（H5/Keras 权重，WHENet 主干基于 EfficientNet）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：**该模块未纳入 docker-compose.yml**，无法通过 `docker-compose up -d` 直接启动；如有需要可单独 `docker build` 后运行。
- 直接运行：在 `d:\Docker\docker\algorithms\head-pose` 下执行 `python server.py`。注意：`server.py` 中默认端口为 `8001`（`os.environ.get('PORT', 8001)`），例如 `PORT=9001 python server.py`。
- 接口（默认端口 8001，任务约定端口 9001）：
  - `GET /health`：健康检查，返回算法名与版本（version 3.0.0，method WHENet+MediaPipe）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理（TensorFlow-CPU），未要求 GPU。