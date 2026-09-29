# face-landmarker

> DEMO2 算法微服务 | 端口 9011 | 容器名 face-landmarker

## 功能
人脸网格关键点检测服务。使用 MediaPipe FaceLandmarker（IMAGE 同步模式，num_faces=1）对镜像翻转后的图像检测单张人脸，输出 478 点人脸网格（468 点人脸 + 10 点虹膜）并支持可视化；可视化中包含基于 PIL 的中文文字绘制工具。

输出 `face_detected`、`landmarks_count`、`face_landmarks`（每点为 `[x, y, z]` 归一化坐标）。

## 目录结构
- `server.py`：Flask 服务入口，包含 FaceLandmarker 加载、关键点检测、人脸网格可视化（tessellation + contours）与 PIL 中文绘制工具。
- 模型文件：`face_landmarker.task`（MediaPipe FaceLandmarker 模型）。
- `Dockerfile`（安装中文字体 `fonts-wqy-microhei`）、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==5.0.0.93
  - numpy==2.0.2
  - mediapipe==0.10.14
  - pillow==11.3.0
  - gunicorn==23.0.0
- 模型文件：`face_landmarker.task`（MediaPipe Task 格式，与 server.py 同目录）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d face-landmarker`（compose 内部注入 `PORT=9011`）。
- 直接运行：在 `d:\Docker\docker\algorithms\face-landmarker` 下执行 `python server.py`，可用环境变量指定端口，例如 `PORT=9011 python server.py`（默认 PORT=9011）。
- 接口（默认端口 9011）：
  - `GET /health`：健康检查，返回 `{"status": "ok"}`。
  - `POST /reset`：为保持接口一致性保留（FaceLandmarker 为无状态同步模式，无需重置）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理，未要求 GPU。