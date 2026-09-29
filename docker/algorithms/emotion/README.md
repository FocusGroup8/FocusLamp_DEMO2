# emotion-detector

> DEMO2 算法微服务 | 端口 9004 | 容器名 emotion-detector

## 功能
面部情绪识别服务。接收 base64 图像帧，先做镜像翻转，再经 EmotionDetectorProvider（EmotiEffLib，MTCNN 人脸检测 + ENET 情绪分类）识别面部情绪。

输出情绪类别 `type`（happiness / sadness / anger / disgust / fear / surprise / contempt / neutral，另有无脸时 `no_face`、模型未加载时 `no_model`、异常时 `error`）、置信度 `confidence`，以及由 `EMOTION_TO_RATING` 映射得到的 0-100 分值 `rating`。

## 目录结构
- `server.py`：Flask 服务入口，负责模型加载、情绪识别、情绪→分值映射与可视化。
- `core/providers/emotion_detector/`：`base.py`（接口定义）、`emotiefflib.py`（EmotiEffLib 实现，即 EmotionDetectorProvider）。
- `core/utils/`：`settings.py`、`logger.py`（配置与日志工具）。
- `config/emotion_detector.yaml`：检测器配置（`device: "cpu"`）。
- 模型文件：`enet_b0_8_va_mtl.onnx`。
- `Dockerfile`、`requirements.txt`、`.dockerignore`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.13.0.92
  - numpy==2.0.2
  - pyyaml==6.0.3
  - pillow==11.3.0
  - onnxruntime==1.19.2
  - facenet-pytorch==2.5.3（MTCNN 人脸检测）
  - emotiefflib==1.1.1（情绪分类）
  - loguru==0.7.3
  - gunicorn==23.0.0
- 模型文件：`enet_b0_8_va_mtl.onnx`（ONNX，ENET-B0 8 类情绪模型）；构建时会被复制到 `/root/.emotiefflib/` 供 EmotiEffLib 加载。
- 外部服务/密钥：无（不调用任何外部 API）。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d emotion-detector`（compose 内部注入 `PORT=9004`）。
- 直接运行：在 `d:\Docker\docker\algorithms\emotion` 下执行 `python server.py`，可用环境变量指定端口，例如 `PORT=9004 python server.py`（默认 PORT=9004）。
- 接口（默认端口 9004）：
  - `GET /health`：健康检查，返回算法名与版本信息（version 4.0.0，method EmotiEffLib_MTCNN）。
  - `POST /detect`：返回检测结果。
  - `POST /detect_visualize`：返回检测结果与可视化图像。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，纯 CPU 推理（配置 `device: cpu`），未要求 GPU。