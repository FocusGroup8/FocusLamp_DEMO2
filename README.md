# FocusLamp DEMO2 — 算法端

FocusLamp「专注灯」系统的**第二版算法端**。接收 ESP32-P4 灯头板（或本地摄像头）的画面，在 PC 端以 Docker Compose 微服务方式运行多路算法，完成人脸 / 情绪 / 疲劳 / 专注度 / 姿态 / 手势 / 玩游戏等检测，结果一路渲染到 Web UI，一路通过 WebSocket 回传给灯头板驱动灯具反馈。

> 本文档为该仓库的入口索引。完整的交接说明见 [docs/交接文档.md](docs/交接文档.md)。
>
> DEMO1（第一版算法端）见独立仓库：[FocusGroup8/FocusLamp](https://github.com/FocusGroup8/FocusLamp) 的 `FocusLamp_DEMO1_Algorithm` 分支。

## 仓库结构

```
/
├── docker/                  # 算法微服务工程（Docker Compose 编排入口）
│   ├── docker-compose.yml   # 服务编排（端口、环境变量、健康检查、资源限制）
│   ├── .env.example         # 密钥模板（.env 不提交）
│   ├── README.md            # 工程总说明与 Docker 使用指南
│   ├── KNOWLEDGE_BASE.md    # 问题与解决方案知识库
│   ├── 算法接口文档.md       # 算法服务接口契约
│   ├── main-client/         # 主控客户端（拉流 + 调度 + Web UI + 结果回传）
│   ├── camera-capture/      # 本地摄像头采集（可选）
│   └── algorithms/          # 算法微服务集合
├── algorithm/
│   └── FocusDetector/       # 单机版专注/行为检测器（非容器，v0.2.0）
└── docs/
    └── 交接文档.md          # 交接文档（7 章：版本关系 / 结构索引 / 负责人 / 文档链接 / 仓库目录 / 开源依赖 / 运行流程）
```

## 模块索引

| 模块 | 端口 | 职责 | 文档 |
|------|------|------|------|
| main-client | 9000（宿主机） | 主控：拉流、调度算法、Web UI、结果回传 ESP32 | [README](docker/main-client/README.md) |
| camera-capture | 9007（宿主机，`profile: camera`） | 本地摄像头采集并广播 JPEG | [README](docker/camera-capture/README.md) |
| focus-detector | 9003（仅容器内） | 专注度 / 参与度 + 番茄钟 | [README](docker/algorithms/focus/README.md) |
| emotion-detector | 9004（仅容器内） | 情绪识别 | [README](docker/algorithms/emotion/README.md) |
| fatigue-detector | 9005（仅容器内） | 疲劳检测 | [README](docker/algorithms/fatigue/README.md) |
| face-detector | 9006（仅容器内） | 人脸检测 + 关键点 | [README](docker/algorithms/face-detector/README.md) |
| gesture-mantis-detector | 9009（仅容器内） | 手势 + 组合手势 | [README](docker/algorithms/gesture-mantis/README.md) |
| pose-detector | 9010（仅容器内） | 姿态 + 颈部运动 | [README](docker/algorithms/pose-detector/README.md) |
| face-landmarker | 9011（仅容器内） | 人脸网格 | [README](docker/algorithms/face-landmarker/README.md) |
| face-position | 9012（仅容器内） | 面部空间坐标 | [README](docker/algorithms/face-position/README.md) |
| vlm-visual-qa | 9013（仅容器内） | 豆包 VLM 视觉问答 | [README](docker/algorithms/vlm-visual-qa/README.md) |
| vlm-game-detector | 9014（仅容器内） | 玩游戏检测 | [README](docker/algorithms/vlm-game-detector/README.md) |
| gesture（遗留） | 9002（未启用） | 手势关键点，参考实现 | [README](docker/algorithms/gesture/README.md) |
| head-pose（遗留） | 9001（未启用） | 头部姿态，参考实现 | [README](docker/algorithms/head-pose/README.md) |
| FocusDetector（单机版） | — | 本地摄像头专注/行为检测演示 | [README](algorithm/FocusDetector/README.md) |

> 除 `main-client`(9000) 与 `camera-capture`(9007) 外，算法端口均为 Docker **内部网络**端口，容器间通过服务名互访（如 `http://focus-detector:9003`）。

## 快速开始

```powershell
cd docker
copy .env.example .env      # 编辑 .env 填入 ARK_API_KEY（vlm-* 服务需要）
docker-compose build
docker-compose up -d
docker-compose ps
# 浏览器打开 http://localhost:9000
```

如需用 PC 本地摄像头替代 ESP32 画面：

```powershell
docker-compose --profile camera up -d camera-capture
```

单机版 FocusDetector（不依赖 Docker 与 ESP32）：

```powershell
cd algorithm/FocusDetector
pip install -e .
python main.py
```

## 运行流程

```
[ESP32-P4 灯头板 :80]
   ├─ /camera ──► {"type":"frame_header","seq":N} + 二进制 JPEG
   │                        ▼
   │              [main-client :9000]  ←──(或── camera-capture :9007)
   │                        ▼  POST /detect_visualize
   │          focus · emotion · fatigue · face-detector · gesture-mantis
   │          pose · face-landmarker · face-position · vlm-visual-qa · vlm-game-detector
   │                        ▼
   │              [Web UI http://localhost:9000]
   └── /algo ◄──────────────┘  JSON-RPC 2.0 tools.call → algorithm.result
                               （每 ALGO_SEND_INTERVAL 默认 1.0s 回传一次）
```

协议字段、`judgment` / `trigger_source` 取值等细节见 [docs/交接文档.md](docs/交接文档.md) 第 7 章。

## 入库约定

- `.env`（含真实 `ARK_API_KEY`）**不提交**，仅提交 `.env.example`；部署时自行创建并填写。
- 大体积模型文件（`.onnx` / `.pt` / `.pth` / `.task` / `.h5` / `.dat` / `.tflite`）**不提交**，需另行获取，清单见 [docs/交接文档.md](docs/交接文档.md) 第 6.4 节。
- 构建产物与生成物不提交：`__pycache__/`、`build/`、`dist/`、`managed_components/`、日志与临时文件。