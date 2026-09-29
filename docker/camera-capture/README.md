# 本地摄像头采集服务（camera-capture）

> DEMO2 | 端口 9007

## 功能

使用 OpenCV 打开本地 USB 摄像头（默认 `CAMERA_INDEX=0`，采集分辨率 640x480），按设定 FPS 循环采集，将每帧编码为 JPEG（`JPEG_QUALITY=80`），并通过 WebSocket 向所有客户端广播「`frame_header` 文本帧 + JPEG 二进制帧」。

它是 ESP32-P4 摄像头流的**本地替代输入源**：当没有 ESP32-P4 灯头板时，`main-client` 可将 `INPUT_SOURCE` 切换为 `camera`，从本服务获取视频帧。本服务自身不缓存帧，仅做采集与广播，且相机仅在被订阅时才有意义（无客户端时仍持续采集但不发送）。

## 目录结构

```
camera-capture/
├── main.py            # 采集与 WebSocket 广播主程序
├── Dockerfile
└── requirements.txt
```

## 依赖

```
numpy==1.26.4
opencv-python-headless==4.11.0.86
websockets==12.0
```

## 运行方式

本服务在 `docker/docker-compose.yml` 中以 `profiles: camera` 标记，**默认不随 `docker compose up` 启动**，需显式启用：

```bash
cd docker
docker compose --profile camera up -d camera-capture
# 或单独启动
docker compose up -d camera-capture
```

直接运行（需已安装 `requirements.txt` 依赖，且宿主机有可用摄像头）：

```bash
python main.py
```

### 环境变量

| 变量 | 说明 / 默认值 |
| --- | --- |
| `PORT` | WebSocket 监听端口，默认 9007 |
| `CAMERA_INDEX` | OpenCV 摄像头索引，默认 0 |
| `FPS` | 采集帧率，docker-compose 设定为 30（`main.py` 代码内默认值为 60） |
| `JPEG_QUALITY` | JPEG 编码质量，默认 80 |

## 对应硬件

- 需要**本地 USB 摄像头**（默认索引 0，可用 `CAMERA_INDEX` 调整）。
- Windows 上通过 docker-compose 运行时，`main-client` 侧需使用 `host.docker.internal` 访问宿主机；本服务容器内访问宿主 USB 摄像头依赖宿主设备映射。

## 对外接口或协议

- **WebSocket 服务**：监听 `0.0.0.0:9007`，客户端连接后即可接收广播。
- **广播帧格式**：
  - 文本帧：`{"type": "frame_header", "seq": <递增序号>, "timestamp": <毫秒时间戳>}`
  - 二进制帧：紧跟在文本帧之后的 JPEG 图像数据。
- **心跳**：服务端 `ping_interval=10`、`ping_timeout=20`；客户端可发送 `{"type":"ping"}`，服务端回 `{"type":"pong"}`。
- 多个客户端会同时收到相同的帧广播；发送失败的客户端会被自动移除。