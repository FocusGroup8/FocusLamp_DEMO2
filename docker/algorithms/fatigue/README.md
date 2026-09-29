# fatigue-detector

> DEMO2 算法微服务 | 端口 9005 | 容器名 fatigue-detector

## 功能
疲劳检测服务。接收 base64 图像帧（镜像翻转后），使用 dlib 68 点人脸关键点计算 EAR（眼睛纵横比，`EYE_AR_THRESH=0.2`）判断眨眼、MAR（嘴部纵横比，`MOUTH_AR_THRESH=0.5`）判断打哈欠，并通过头部姿态（`HAR_THRESH=0.3`）判断点头；按连续帧计数（`*_CONSEC_FRAMES=3`）确认动作。

输出 `rating`（疲劳评分）、`ear`、`mar`、`har`、`blink_count`、`yawn_count`、`nod_count`、`face_detected` 与 68 点 `landmarks` 坐标。

## 目录结构
- `server.py`：Flask 服务入口，包含 dlib 模型加载、EAR/MAR/点头计算、连续帧计数与可视化。
- 模型文件：`shape_predictor_68_face_landmarks.dat`（dlib 68 点关键点预测器）。
- `Dockerfile`、`requirements.txt`、`.dockerignore`（构建时需 `build-essential` + `cmake` 编译 dlib）。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - dlib==20.0.1
  - scipy==1.13.1
  - gunicorn==23.0.0
- 模型文件：`shape_predictor_68_face_landmarks.dat`（dlib 原生格式）；compose 将其缓存目录挂载到 `/root/.dlib`。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d fatigue-detector`（compose 内部注入 `PORT=9005`，实际运行端口为 9005）。
- 直接运行：在 `d:\Docker\docker\algorithms\fatigue` 下执行 `python server.py`。注意：`server.py` 中默认端口为 `8005`（`os.environ.get('PORT', 8005)`），如需与容器端口一致请显式指定，例如 `PORT=9005 python server.py`。
- 接口（容器运行时默认端口 9005）：
  - `GET /health`：健康检查，返回算法名与版本（version 3.0.0，method dlib_EAR_MAR）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理，未要求 GPU。