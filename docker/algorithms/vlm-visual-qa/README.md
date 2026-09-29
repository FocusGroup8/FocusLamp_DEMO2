# vlm-visual-qa

> DEMO2 算法微服务 | 端口 9013 | 容器名 vlm-visual-qa

## 功能
基于大视觉语言模型的视觉问答服务（火山引擎豆包 Doubao Responses API）。接收图像 + 可选的指尖归一化坐标 `(x, y)`（`fingertip_x` / `fingertip_y`）；若提供坐标，则在画面上以红色圆圈标记该位置，再调用豆包模型描述红圈处的内容，返回文本描述（`answer`）与可视化图像。

用于"用户用食指指向画面某处，由 VLM 描述所指内容"的交互场景。

## 目录结构
- `server.py`：Flask 服务入口，包含 VisualQAEngine（同步/异步调用豆包 API）、红圈标记与可视化、状态管理。
- `Dockerfile`：构建上下文为 `./algorithms`，会一并 `COPY _common/`（公共图像编解码/中文绘制工具）与 `server.py`，并安装中文字体。
- 依赖公共模块：`d:\Docker\docker\algorithms\_common\common.py`。
- `requirements.txt`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - opencv-python-headless==4.11.0.86
  - numpy==1.26.4
  - pillow==11.3.0
  - openai==2.46.0
  - gunicorn==23.0.0
- 模型文件：无本地模型（推理在云端）。
- 外部服务/密钥：
  - 火山引擎 Ark API Key，通过环境变量 `ARK_API_KEY` 注入（**严禁硬编码或提交到仓库**，在 `d:\Docker\docker\.env` 中配置）。
  - `VLM_MODEL_ENDPOINT`：默认 `doubao-seed-2-0-mini-260428`。
  - `VLM_BASE_URL`：默认 `https://ark.cn-beijing.volces.com/api/v3`。
  - 受 `JPEG_QUALITY`（默认 50）、`MAX_IMAGE_WIDTH`（默认 640）、`MARKER_RADIUS`（默认 20）等环境变量控制。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d vlm-visual-qa`（compose 内部注入 `PORT=9013` 与 `ARK_API_KEY` 等）。
- 直接运行：在 `d:\Docker\docker\algorithms\vlm-visual-qa` 下执行 `python server.py`，需先设置 `ARK_API_KEY`，例如 `PORT=9013 ARK_API_KEY=xxx python server.py`（默认 PORT=9013）。
- 接口（默认端口 9013）：
  - `GET /health`：健康检查。
  - `POST /detect`：仅返回检测结果。
  - `POST /detect_visualize`：返回结果 + 可视化图像。
  - `POST /reset`：重置状态。
  - 另在代码中提供 `POST /trigger_async`（异步触发）与 `GET /state`（查询状态）。
  - 请求体：`{"image": "<base64>", "fingertip_x": <0~1>, "fingertip_y": <0~1>}`（坐标可选）。

## 对应硬件
无特殊硬件依赖。输入为 base64 编码图像，本体推理为纯 CPU；但**需要外网访问豆包 API**（`ark.cn-beijing.volces.com`）才能返回结果。