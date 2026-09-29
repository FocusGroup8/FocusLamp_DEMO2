# face-position

> DEMO2 算法微服务 | 端口 9012 | 容器名 face-position

## 功能
面部空间坐标估算服务。以 FaceLandmarker 的鼻尖、双眼外角关键点，结合外部传入的 PoseLandmarker 双肩/鼻关键点，估算面部在相机坐标系下的三维位置：

- `x_cm`：水平偏移（+ 向右 / - 向左）
- `y_cm`：垂直偏移（+ 向上 / - 向下）
- `z_cm`：鼻尖相对双肩的深度差（+ 低头 / - 后仰）
- `distance_cm`：到摄像头的绝对距离估算
- `confidence`：置信度（0~1）；`valid`：是否有效输出

估计器内含物理先验（瞳距 6.3cm、头长 23cm、摄像头 FOV 60°）、一阶低通滤波（`SMOOTHING_ALPHA=0.3`）、异常跳变剔除（`OUTLIER_JUMP_THRESHOLD_CM=30.0`）与连续无效帧重置（`INVALID_RESET_FRAMES=10`）。注意：位置估计要求人脸与姿态同时有效，需请求中提供 `pose_landmarks`。

## 目录结构
- `server.py`：Flask 服务入口，包含 FaceLandmarker 加载、`FacePositionEstimator` 估计器、可视化（face mesh + 中文位置信息）与 PIL 中文绘制工具。
- 模型文件：`face_landmarker.task`（MediaPipe FaceLandmarker 模型，路径可由环境变量 `MODEL_PATH` 覆盖，默认与 server.py 同目录）。
- `Dockerfile`（安装中文字体 `fonts-wqy-microhei`）、`requirements.txt`、`.dockerignore`。
- 设计说明见 `d:\Docker\algorithm\FocusDetector\docs\face_position_design.md`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==5.0.0.93
  - numpy==2.0.2
  - mediapipe==0.10.14
  - pillow==11.3.0
  - gunicorn==23.0.0
- 模型文件：`face_landmarker.task`（MediaPipe Task 格式）。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d face-position`（compose 内部注入 `PORT=9012`）。
- 直接运行：在 `d:\Docker\docker\algorithms\face-position` 下执行 `python server.py`，可用环境变量指定端口与模型路径，例如 `PORT=9012 MODEL_PATH=face_landmarker.task python server.py`（默认 PORT=9012）。
- 接口（默认端口 9012）：
  - `GET /health`：健康检查，返回 `{"status": "ok"}`。
  - `POST /reset`：重置估计器滤波/异常剔除状态。
  - `POST /detect`：返回检测结果（位置坐标由 `pose_landmarks` 输入决定，未提供时仅运行人脸检测、不估计位置）。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>", "pose_landmarks": [[x, y, z, visibility], ...]}`（`pose_landmarks` 为可选，来自 pose-detector 输出的 33 点）。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理，未要求 GPU。