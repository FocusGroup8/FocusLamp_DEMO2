# vlm-game-detector

> DEMO2 算法微服务 | 端口 9014 | 容器名 vlm-game-detector

## 功能
"是否在玩游戏"检测服务，采用本地检测 + 云端 VLM 验证的两级流程：

1. YOLOv8 检测画面中的 phone / mouse / keyboard（COCO 类别）。
2. MediaPipe HandLandmarker 检测手部关键点。
3. 判断手部关键点是否落在目标 bbox 内（接触检测）。
4. 手机/电脑场景任一接触 → 触发豆包 API 验证是否在玩游戏。
5. 冷却机制防止重复触发。
6. 返回可视化图像（YOLO 检测框 + 接触点 + 状态栏 + API 结果）。

输出 `judgment`（是/否）、`trigger_source`（触发来源，代码取值 `phone` / `computer`）、`reason`（判定理由）、`cooldown_remaining`（冷却剩余秒数）等。

## 目录结构
- `server.py`：Flask 服务入口，包含 YOLO 检测、手部接触判断、豆包 API 调用与冷却控制、可视化。
- `Dockerfile`：构建上下文为 `./algorithms`，会一并 `COPY _common/`、`hand_landmarker.task`、`yolo26s.pt`、`server.py`，并安装中文字体。
- 依赖公共模块：`d:\Docker\docker\algorithms\_common\common.py`。
- 模型文件：`yolo26s.pt`（Ultralytics YOLO）、`hand_landmarker.task`（MediaPipe HandLandmarker）。
- `requirements.txt`。

## 依赖
- Python 依赖（requirements.txt）：
  - flask==3.1.3
  - openai==2.46.0
  - ultralytics==8.4.100
  - mediapipe==0.10.35
  - numpy==1.26.4
  - opencv-python-headless==4.11.0.86
  - requests==2.32.5
  - gunicorn==23.0.0
- 模型文件：
  - `yolo26s.pt`（Ultralytics YOLO 权重，默认路径由 `YOLO_MODEL` 指定）。
  - `hand_landmarker.task`（MediaPipe 手部关键点模型，容器内默认路径 `/app/hand_landmarker.task`，可由 `HAND_LANDMARKER_PATH` 覆盖）。
- 外部服务/密钥：
  - 火山引擎 Ark API Key，通过环境变量 `ARK_API_KEY` 注入（**严禁硬编码或提交到仓库**）。
  - `VLM_MODEL_ENDPOINT`：默认 `doubao-seed-2-0-mini-260428`；`VLM_BASE_URL`：默认 `https://ark.cn-beijing.volces.com/api/v3`。
  - 其它参数：`TRIGGER_COOLDOWN`（代码默认 5.0，compose 中设为 10）、`PHONE_CONFIDENCE_THRESHOLD`、`PC_CONFIDENCE_THRESHOLD`、`HAND_CONTACT_MARGIN` 等。

## 运行方式
- 容器方式：在 `d:\Docker\docker` 目录执行 `docker-compose up -d vlm-game-detector`（compose 内部注入 `PORT=9014`、`ARK_API_KEY`、`TRIGGER_COOLDOWN=10` 等）。
- 直接运行：在 `d:\Docker\docker\algorithms\vlm-game-detector` 下执行 `python server.py`，需先设置 `ARK_API_KEY`，例如 `PORT=9014 ARK_API_KEY=xxx python server.py`（默认 PORT=9014）。
- 接口（默认端口 9014）：
  - `GET /health`：健康检查。
  - `POST /detect`：仅返回检测结果。
  - `POST /detect_visualize`：返回结果 + 可视化图像。
  - `POST /reset`：重置状态。
  - 另在代码中提供 `GET /state`（查询状态）。
  - 请求体：`{"image": "<base64>"}`。

## 对应硬件
无特殊硬件依赖。本地 YOLO + MediaPipe 推理可在 CPU 运行；但**需要外网访问豆包 API** 才能完成游戏行为验证。