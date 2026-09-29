# 主控客户端（main-client）

> DEMO2 | 端口 9000

## 功能

系统主控。作为 DEMO2 的调度中枢，负责视频帧接收、算法分发、结果回传与 Web 前端展示：

1. **接收视频帧**：以 WebSocket 客户端连接 ESP32-P4 灯头板的 `/camera` 端点，接收「二进制 JPEG 帧 + `{"type":"frame_header","seq":N}` 文本帧」；或将输入源切换为本地 camera-capture 服务接收帧。
2. **调度算法服务**：按帧调度各算法容器的 `/detect_visualize` 端点，支持 `skip_frames` 降频、`forced_algorithms` 强制启用、503 忙碌跳过，以及基于 `/health` 的健康检查（服务不可用时自动停止分发，恢复后续传）。
3. **Web 前端**：内置前端页面（`static/index.html`），展示各算法的可视化结果，并支持在线切换输入源、下发算法启停控制。
4. **结果回传 ESP32**：通过独立的 `/algo` WebSocket 连接，以 JSON-RPC 2.0 的 `tools.call`（工具名 `algorithm.result`）将算法结果回传给 ESP32。

**输入源切换**：环境变量 `INPUT_SOURCE`（`esp32p4` 或 `camera`），也可通过 HTTP `POST /input_source` 或前端 WebSocket 的 `switch_input` 消息在线切换。

主要逻辑：
- 帧调度 `_schedule_algorithms`：`face_detector` 与 `focus` 每帧调用；其余算法按各自 `skip_frames` 计数器降频（`other_algo_interval` 节流 0.1s）。
- 帧预处理：`esp32p4` 源在独立线程执行「水平翻转 + JPEG 重编码」，`camera` 源直接透传。
- 帧统计：每 `FPS_STATS_WINDOW`（30）帧重置平均耗时统计，并按秒输出 Detect FPS、平均耗时、`frame_seq`、丢帧（drops）与乱序（out_of_order）计数。
- VLM 触发：`Pointing_Up` 手势持续 `VLM_POINTING_HOLD_THRESHOLD` 秒后触发 `vlm_visual_qa`；`vlm_game_detector` 通过强制启用每帧调用（内部自判是否请求豆包 API）。

### 算法超时配置（`ALGORITHM_TIMEOUTS`）

| 算法 | 超时 |
| --- | --- |
| `focus` | 8.0s |
| `vlm_visual_qa` | 30.0s |
| `vlm_game_detector` | 8.0s |
| 其余（默认 `ALGORITHM_TIMEOUT_DEFAULT`） | 3.0s |

### 覆盖的算法枚举（`AlgoName`）

`focus`、`emotion`、`fatigue`、`face_detector`、`gesture_mantis`、`pose`、`face_landmarker`、`face_position`、`vlm_visual_qa`、`vlm_game_detector`。

## 目录结构

```
main-client/
├── main.py            # 主程序：帧接收、算法调度、Web 服务、结果回传
├── static/
│   └── index.html     # Web 前端页面
├── Dockerfile
├── requirements.txt
└── test_ws.py         # WebSocket 连通性测试脚本
```

## 依赖

```
websockets==15.0.1
aiohttp==3.13.5
opencv-python-headless==4.13.0.92
numpy==2.0.2
```

## 运行方式

容器（推荐，由 `docker/docker-compose.yml` 编排）：

```bash
cd docker
docker compose up -d main-client
```

启动后访问 Web UI：<http://localhost:9000>

直接运行（需已安装 `requirements.txt` 依赖）：

```bash
python main.py
```

### 关键环境变量

| 变量 | 说明 / 默认值 |
| --- | --- |
| `PORT` | Web 服务端口，默认 9000 |
| `INPUT_SOURCE` | 输入源，`esp32p4` 或 `camera` |
| `ESP32P4_IP` / `ESP32P4_PORT` | ESP32-P4 灯头板地址 / 端口（默认 80） |
| `CAMERA_CAPTURE_HOST` / `CAMERA_CAPTURE_PORT` | 本地采集服务地址 / 端口（默认 9007） |
| `FOCUS_URL` | focus 算法服务地址 |
| `EMOTION_URL` | emotion 算法服务地址 |
| `FATIGUE_URL` | fatigue 算法服务地址 |
| `FACE_DETECTOR_URL` | face_detector 算法服务地址 |
| `GESTURE_MANTIS_URL` | gesture_mantis 算法服务地址 |
| `POSE_URL` | pose 算法服务地址 |
| `FACE_LANDMARKER_URL` | face_landmarker 算法服务地址 |
| `FACE_POSITION_URL` | face_position 算法服务地址 |
| `VLM_VISUAL_QA_URL` | vlm_visual_qa 算法服务地址 |
| `VLM_GAME_DETECTOR_URL` | vlm_game_detector 算法服务地址 |
| `VLM_POINTING_HOLD_THRESHOLD` | Pointing_Up 触发 VLM 视觉问答的持续时长（秒） |
| `VLM_GAME_SAMPLE_INTERVAL` | VLM 游戏检测采样间隔 |
| `WEBSOCKET_PING_INTERVAL` / `WEBSOCKET_PING_TIMEOUT` / `WEBSOCKET_CLOSE_TIMEOUT` | WebSocket 心跳与关闭超时 |
| `ALGORITHM_MAX_CONCURRENCY` | 算法调用并发上限（默认 4） |
| `HEALTH_CHECK_INTERVAL` | 算法健康检查周期（默认 30s） |
| `ALGO_SEND_INTERVAL` | 向 ESP32 回传算法结果的周期（默认 1.0s） |

## 对应硬件

- 需可访问 ESP32-P4 灯头板（WebSocket，默认端口 80）或本地 USB 摄像头（经 camera-capture 服务）。
- 本服务**不直接占用摄像头**：仅作为 WebSocket 客户端消费视频帧。
- 摄像头采集由 camera-capture（9007）或 ESP32-P4 灯头板完成。

## 对外接口或协议

### 入站（本服务提供）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | Web 前端页面（`static/index.html`） |
| GET | `/frame` | 返回最新一帧（base64 JPEG） |
| GET | `/results` | 返回各算法最新原始结果 |
| GET | `/video_feed` | MJPEG 视频流（`multipart/x-mixed-replace`） |
| GET | `/input_source` | 查询当前输入源与可用输入源列表 |
| POST | `/input_source` | 切换输入源，body `{"source": "esp32p4"|"camera"}` |
| POST | `/algorithm/{algorithm}` | 代理转发到对应算法容器的 `/detect_visualize` |
| WS | `/ws` | 前端 WebSocket：推送 `frame_header` 文本帧 + JPEG 二进制帧 + `algorithm_results` JSON；接收 `ping`/`switch_input`/`control`/`report_config` |

### 出站（本服务作为客户端）

| 方向 | 端点 | 说明 |
| --- | --- | --- |
| 连接 | `ws://<ESP32P4_IP>:<ESP32P4_PORT>/camera` | 接收视频帧（二进制 JPEG + `frame_header` 文本帧） |
| 连接 | `ws://<CAMERA_CAPTURE_HOST>:<CAMERA_CAPTURE_PORT>` | 接收本地采集服务的视频帧 |
| 连接 | `ws://<ESP32P4_IP>:<ESP32P4_PORT>/algo` | 回传算法结果，JSON-RPC 2.0 `tools.call` / `algorithm.result` |
| POST | `<ALGO_URL>/detect_visualize` | 调度算法服务，body `{"image": <base64>, "timestamp": <ms>, ...}` |
| GET | `<ALGO_URL>/health` | 算法服务健康检查 |