# face-detector

> DEMO2 算法微服务 | 端口 9006 | 容器名 face-detector

## 功能
人脸检测与关键点服务。使用 SCRFD（slim）ONNX 模型（`weights/slim_Final.onnx`，配置 `cfg_slim`：`min_sizes`/`steps`/`variance`/`image_size=300`）检测人脸，输出人脸 bbox 与 5 个关键点（左右眼、鼻尖、左右嘴角），并带有 DEADZONE 去抖（`DEADZONE_THRESHOLD=20.0`，抑制相邻帧框体抖动）。

## 目录结构
- `server.py`：Flask 服务入口，包含 ONNX 会话加载（自动选择 CUDA/CPU provider）、PriorBox/解码/NMS 等 SCRFD 后处理、去抖与可视化。
- `weights/`：`slim_Final.onnx`（ONNX 模型本体）、`slim_Final.onnx.data`（外部权重数据）、`slim_Final.pth`（PyTorch 原始权重）。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - onnxruntime==1.19.2
  - gunicorn==23.0.0
- 模型文件：`weights/slim_Final.onnx`（ONNX，SCRFD slim；含同名 `.onnx.data` 外部权重）。
- 外部服务/密钥：无（不调用任何外部 API）。
- 运行时会自动检测可用 provider：若存在 `CUDAExecutionProvider` 则优先使用 GPU，否则回退 `CPUExecutionProvider`。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d face-detector`（compose 内部注入 `PORT=9006`，实际运行端口为 9006）。
- 直接运行：在 `d:\Docker\docker\algorithms\face-detector` 下执行 `python server.py`。注意：`server.py` 中默认端口为 `8006`（`os.environ.get('PORT', 8006)`），如需与容器端口一致请显式指定，例如 `PORT=9006 python server.py`。
- 接口（容器运行时默认端口 9006）：
  - `GET /health`：健康检查，返回算法名与版本（version 3.0.0，method Face-Detector-1MB_Slim_ONNX）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像；有 GPU 时可自动启用 CUDA 加速，纯 CPU 亦可运行。