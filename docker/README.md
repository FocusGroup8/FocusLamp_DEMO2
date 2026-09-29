# ESP32P4 Algorithm Visualization System

## 项目概述

基于 Docker 的人脸识别算法可视化系统，包含 7 个微服务容器。

## 文档索引

| 文档 | 说明 |
|------|------|
| [README.md](README.md) | 项目说明、Docker 使用指南 |
| [KNOWLEDGE_BASE.md](KNOWLEDGE_BASE.md) | 问题与解决方案知识库 |
| [docker-compose.yml](docker-compose.yml) | Docker Compose 配置文件 |

## 当前状态

### 已完成
- [x] Docker 镜像构建（7 个服务）
- [x] docker-compose.yml 优化（删除 version 字段，添加 volume 挂载）
- [x] Dockerfile 优化（清理缓存、.dockerignore）
- [x] 前端页面更新（移除 MJPEG 流卡片）
- [x] WebSocket 连接稳定性优化
- [x] ESP32P4 双缓冲 JPEG 输出优化
- [x] 帧序号验证和乱帧检测
- [x] 前端渲染优化（requestAnimationFrame + 缓存图像对象）
- [x] 算法可视化端点修复（所有算法统一使用 /detect_visualize）
- [x] ESP32P4 结果解析 bug 修复（结果在 result 子对象中）
- [x] head_pose 数据发送给 ESP32P4

### 当前状态
- [x] 所有算法容器正常运行
- [x] 前端可视化正常显示
- [x] ESP32P4 连接稳定

## 服务架构

| 服务 | 端口 | 状态 |
|------|------|------|
| main-client | 9000 | ✅ 运行中 |
| head-pose-detector | 9001 | 可选 |
| gesture-detector | 9002 | 可选 |
| focus-detector | 9003 | 可选 |
| emotion-detector | 9004 | 可选 |
| fatigue-detector | 9005 | 可选 |
| face-detector | 9006 | ✅ 运行中 |

---

## Docker 使用指南

### 前置要求
- Windows 10/11 专业版或企业版
- Docker Desktop 已安装并运行
- 至少 8GB 可用内存
- 至少 20GB 可用磁盘空间

### 基础命令集

#### 1. 启动 Docker Desktop
```bash
# 确保 Docker Desktop 正在运行
# Windows: 手动启动 Docker Desktop 应用程序
# 或使用命令行启动（需要管理员权限）
"C:\Program Files\Docker\Docker\Docker Desktop.exe"
```

#### 2. 构建镜像
```bash
# 进入项目目录
cd d:\Internship\new\docker

# 构建所有服务镜像
docker-compose build

# 构建单个服务镜像
docker-compose build main-client
docker-compose build face-detector

# 强制重新构建（不使用缓存）
docker-compose build --no-cache
```

#### 3. 启动服务
```bash
# 启动所有服务（后台运行）
docker-compose up -d

# 启动指定服务（推荐用于调试）
docker-compose up -d main-client face-detector

# 启动单个算法服务
docker-compose up -d head-pose-detector
docker-compose up -d gesture-detector
docker-compose up -d focus-detector
docker-compose up -d emotion-detector
docker-compose up -d fatigue-detector

# 前台运行（查看实时日志）
docker-compose up
```

#### 4. 停止服务
```bash
# 停止所有服务
docker-compose down

# 停止指定服务
docker-compose stop main-client
docker-compose stop face-detector

# 停止并删除容器、网络、卷
docker-compose down -v
```

#### 5. 重启服务
```bash
# 重启所有服务
docker-compose restart

# 重启指定服务
docker-compose restart main-client
docker-compose restart face-detector
```

#### 6. 查看服务状态
```bash
# 查看所有服务状态
docker-compose ps

# 查看所有容器（包括停止的）
docker ps -a

# 查看容器详细信息
docker inspect main-client
```

#### 7. 查看日志
```bash
# 查看所有服务日志
docker-compose logs

# 查看指定服务日志
docker-compose logs main-client
docker-compose logs face-detector

# 实时跟踪日志
docker-compose logs -f main-client

# 查看最近100行日志
docker-compose logs --tail=100 main-client
```

#### 8. 进入容器调试
```bash
# 进入运行中的容器
docker exec -it main-client bash

# 进入容器并执行命令
docker exec main-client python --version

# 以root用户进入容器
docker exec -it -u root main-client bash
```

#### 9. 清理资源
```bash
# 删除所有停止的容器
docker container prune

# 删除所有未使用的镜像
docker image prune -a

# 删除所有未使用的卷
docker volume prune

# 删除所有未使用的网络
docker network prune

# 一键清理所有未使用资源
docker system prune -a
```

#### 10. 网络相关
```bash
# 查看Docker网络
docker network ls

# 查看网络详细信息
docker network inspect docker_default

# 创建自定义网络
docker network create my_network

# 连接容器到网络
docker network connect my_network main-client
```

### 服务管理最佳实践

#### 启动顺序（推荐）
```bash
# 1. 启动核心服务
docker-compose up -d main-client face-detector

# 2. 根据需要启动其他算法服务
docker-compose up -d head-pose-detector
docker-compose up -d gesture-detector
docker-compose up -d focus-detector
docker-compose up -d emotion-detector
docker-compose up -d fatigue-detector
```

#### 调试模式启动
```bash
# 前台运行单个服务（便于查看日志）
docker-compose up main-client

# 或使用日志跟踪
docker-compose up -d main-client
docker-compose logs -f main-client
```

#### 配置更新后重启
```bash
# 修改 docker-compose.yml 或环境变量后
docker-compose down
docker-compose up -d

# 或直接重启
docker-compose restart main-client
```

### 环境变量配置

#### docker-compose.yml 环境变量
```yaml
environment:
  - PORT=9000                          # 服务端口
  - ESP32P4_IP=192.168.55.143          # ESP32P4 设备IP
  - ESP32P4_PORT=8080                  # ESP32P4 WebSocket端口
  - HEAD_POSE_URL=http://head-pose-detector:9001
  - GESTURE_URL=http://gesture-detector:9002
  - FOCUS_URL=http://focus-detector:9003
  - EMOTION_URL=http://emotion-detector:9004
  - FATIGUE_URL=http://fatigue-detector:9005
  - FACE_DETECTOR_URL=http://face-detector:9006
  - WEBSOCKET_PING_INTERVAL=10         # WebSocket心跳间隔（秒）
  - WEBSOCKET_PING_TIMEOUT=20          # WebSocket超时时间（秒）
  - WEBSOCKET_CLOSE_TIMEOUT=10         # WebSocket关闭超时（秒）
```

#### 修改ESP32P4 IP地址
```bash
# 方法1: 编辑 docker-compose.yml
# 方法2: 使用环境变量文件
echo "ESP32P4_IP=192.168.55.143" > .env
docker-compose up -d
```

### 网络配置说明

#### 网络模式
- **bridge模式**（当前使用）：容器有独立IP，通过端口映射访问
- **host模式**：容器使用宿主机网络，无需端口映射（Windows不支持）

#### 端口映射
```yaml
ports:
  - "9000:9000"  # 宿主机端口:容器端口
```

#### 容器间通信
- 容器通过服务名互相访问：`http://face-detector:9006`
- 不需要使用IP地址，Docker内部DNS自动解析

### 访问前端
http://localhost:9000

---

## 错误总结与解决方案

### 1. Docker Desktop 未启动

**错误现象：**
```
error during connect: This error may indicate that the docker daemon is not running.
```

**原因：**
Docker Desktop 应用程序未运行

**解决方案：**
```bash
# 手动启动 Docker Desktop 应用程序
# 或使用命令行启动（需要管理员权限）
"C:\Program Files\Docker\Docker\Docker Desktop.exe"

# 等待 Docker Desktop 完全启动（约30-60秒）
# 验证 Docker 是否运行
docker ps
```

---

### 2. Docker 镜像源 DNS 解析问题

**错误现象：**
```
ERROR [internal] load metadata for docker.io/library/python:3.9-slim
failed to solve with frontend dockerfile.v0: failed to create LLB definition: failed to authorize: rpc error: code = Unknown desc = connection error
```

**原因：**
- Docker 默认镜像源访问慢或被墙
- DNS 解析失败

**解决方案：**
```bash
# 方法1: 配置 Docker 镜像加速器
# 编辑 Docker Desktop 设置 -> Docker Engine
# 添加以下配置：
{
  "registry-mirrors": [
    "https://docker.mirrors.ustc.edu.cn",
    "https://hub-mirror.c.163.com"
  ]
}

# 方法2: 使用国内镜像源
# 在 Dockerfile 中使用阿里云镜像
FROM python:3.9-slim

# 方法3: 手动拉取镜像
docker pull python:3.9-slim
```

---

### 3. OpenCV 缺少系统库 libxcb.so.1

**错误现象：**
```
ImportError: libxcb.so.1: cannot open shared object file: No such file or directory
```

**原因：**
OpenCV 依赖的系统库未安装

**解决方案：**
```dockerfile
# 在 Dockerfile 中添加系统库安装
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender1 \
    libxcb1 \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean
```

**适用文件：**
- `algorithms/emotion/Dockerfile`
- `algorithms/focus/Dockerfile`
- `algorithms/head-pose/Dockerfile`
- `algorithms/face-detector/Dockerfile`
- `algorithms/fatigue/Dockerfile`
- `algorithms/gesture/Dockerfile`

---

### 4. Windows Docker Desktop host 网络模式端口映射问题

**错误现象：**
```
容器启动成功，但无法通过 localhost 访问服务
端口映射不生效
```

**原因：**
- Windows Docker Desktop 不支持 host 网络模式
- host 模式下端口映射无效

**解决方案：**
```yaml
# 使用 bridge 网络模式（默认）
services:
  main-client:
    network_mode: bridge  # 或删除此行，默认就是 bridge
    ports:
      - "9000:9000"  # 端口映射生效

# 不要使用 host 模式
# network_mode: host  # Windows 不支持
```

---

### 5. WebSocket 连接超时

**错误现象：**
```
[INFO] Connecting to ws://192.168.55.143:8080
[ERROR] Connection timeout
```

**原因：**
- ESP32P4 设备 IP 地址错误
- ESP32P4 未连接到 WiFi
- 网络不通

**解决方案：**
```bash
# 1. 确认 ESP32P4 IP 地址
# 查看 ESP32P4 串口输出或路由器管理界面

# 2. 更新 docker-compose.yml 中的 IP 地址
environment:
  - ESP32P4_IP=<正确的IP地址>

# 3. 测试网络连通性
ping 192.168.55.143

# 4. 确保 ESP32P4 已连接 WiFi
# ESP32P4 端配置正确的 WiFi SSID 和密码
```

---

### 6. WebSocket 发送帧错误

**错误现象：**
```
WebSocket send error: [Errno 9] Bad file descriptor
```

**原因：**
- WebSocket 连接已断开
- 发送操作阻塞

**解决方案：**
```python
# 使用非阻塞方式发送
import asyncio

async def send_frame(ws, data):
    try:
        await asyncio.wait_for(ws.send(data), timeout=5.0)
    except asyncio.TimeoutError:
        print("Send timeout")
    except Exception as e:
        print(f"Send error: {e}")

# 添加错误处理和重连机制
```

---

### 7. create_connection() 参数错误

**错误现象：**
```
TypeError: create_connection() got an unexpected keyword argument 'read_limit'
```

**原因：**
- websockets 库版本不支持 read_limit 参数
- 参数名称错误

**解决方案：**
```python
# 移除不支持的参数
async with websockets.connect(
    self.ws_url,
    ping_interval=10,
    ping_timeout=20,
    close_timeout=10,
    max_size=10 * 1024 * 1024
    # 不要使用 read_limit 参数
) as ws:
    # ...
```

---

### 8. 帧跳跃和乱帧问题

**错误现象：**
```
前端显示的帧出现跳跃、闪烁
闪烁的帧是7秒前出现过的帧
```

**原因：**
- 双缓冲切换逻辑问题
- 前端 Canvas 渲染顺序问题
- WebSocket 消息乱序
- 帧序号验证缺失

**解决方案：**

#### ESP32P4 端（C代码）
```c
// 添加帧序号
uint32_t frame_seq = 0;

while (s_stream_running) {
    // 获取帧
    // ...
    
    frame_seq++;
    
    // 发送帧时包含序号
    // 使用双缓冲确保数据一致性
}
```

#### 后端（Python）
```python
# 添加帧序号验证
last_frame_seq = 0

def process_frame(data, seq):
    global last_frame_seq
    
    if seq <= last_frame_seq:
        print(f"跳过旧帧: {seq}, 当前: {last_frame_seq}")
        return
    
    last_frame_seq = seq
    # 处理帧...
```

#### 前端（JavaScript）
```javascript
let lastRenderedSeq = 0;
let pendingFrame = null;
let pendingSeq = 0;
let isRendering = false;

function displayFrameOnCanvas(imageBase64, seq) {
    // 跳过旧帧
    if (seq !== undefined && seq <= lastRenderedSeq) {
        console.log(`跳过旧帧: ${seq}, 当前: ${lastRenderedSeq}`);
        return;
    }
    
    pendingFrame = imageBase64;
    pendingSeq = seq || 0;
    
    if (!isRendering) {
        isRendering = true;
        requestAnimationFrame(renderPendingFrame);
    }
}

function renderPendingFrame() {
    if (!pendingFrame) {
        isRendering = false;
        return;
    }
    
    // 使用缓存图像对象
    if (!cachedImage) {
        cachedImage = new Image();
    }
    
    cachedImage.onload = () => {
        rawCtx.drawImage(cachedImage, 0, 0);
        lastRenderedSeq = pendingSeq;
        pendingFrame = null;
        
        if (pendingFrame) {
            requestAnimationFrame(renderPendingFrame);
        } else {
            isRendering = false;
        }
    };
    cachedImage.src = `data:image/jpeg;base64,${pendingFrame}`;
}
```

---

### 9. Docker Compose 版本警告

**错误现象：**
```
WARN[0000] The "version" field in docker-compose.yml is deprecated
```

**原因：**
Docker Compose V2 不再需要 version 字段

**解决方案：**
```yaml
# 删除 docker-compose.yml 中的 version 字段
# 旧版本：
# version: '3.8'
# services:
#   ...

# 新版本：
services:
  main-client:
    # ...
```

---

### 10. 容器内存不足

**错误现象：**
```
Container killed due to memory limit
```

**原因：**
- 算法服务需要大量内存
- Docker 默认内存限制过低

**解决方案：**
```yaml
# 在 docker-compose.yml 中设置内存限制
services:
  face-detector:
    deploy:
      resources:
        limits:
          memory: 4G
        reservations:
          memory: 2G
```

---

## ESP32P4 配置

### 当前配置
```
IP地址: 192.168.55.143
端口: 8080
WiFi SSID: CU_x7DE
WiFi 密码: kf57ky9s
```

### 修改 IP 地址
编辑 `docker-compose.yml`:
```yaml
environment:
  - ESP32P4_IP=<新的IP地址>
```

### WiFi 配置
ESP32P4 端代码位置: `d:\Internship\new\camera_websocket_stream\`

修改 `main/main.c`:
```c
#define WIFI_SSID "CU_x7DE"
#define WIFI_PASS "kf57ky9s"
```

---

## 性能指标

### 帧率优化前后对比

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 帧间隔 | 450-1000ms | 180-183ms |
| 帧率 | 1-2 FPS | 5.5 FPS |

### 优化措施
1. ESP32P4 双缓冲 JPEG 输出
2. WebSocket 心跳优化
3. 前端 requestAnimationFrame 渲染
4. 帧序号验证和乱帧过滤
5. 图像对象缓存

---

## 文件结构

```
docker/
├── docker-compose.yml          # Docker Compose 配置
├── main-client/                # 主客户端服务
│   ├── Dockerfile
│   ├── main.py                 # 主程序
│   ├── requirements.txt
│   └── static/index.html       # 前端页面
└── algorithms/                 # 算法服务
    ├── emotion/                # 情绪检测
    ├── face-detector/          # 人脸检测
    ├── fatigue/                # 疲劳检测
    ├── focus/                  # 注意力检测
    ├── gesture/                # 手势识别
    └── head-pose/              # 头部姿态
```

---

## ESP32P4 端代码

位置: `d:\Internship\new\camera_websocket_stream\`

### 双缓冲优化
已添加双缓冲 JPEG 输出，修改文件：
- `main/main.c`

### 关键优化点
1. 双缓冲切换逻辑
2. 帧序号生成
3. WebSocket 发送优化
4. 任务优先级调整

---

## 待解决问题

### 1. 错帧问题（深入定位中）
- 现象：闪烁的帧是7秒前出现过的帧
- 已采取措施：
  - 添加帧序号验证
  - 优化前端渲染逻辑
  - 使用 requestAnimationFrame
- 待排查：
  - ESP32P4 端缓冲区管理
  - WebSocket 消息队列
  - 网络延迟影响

---

## 下一步计划

1. 深入定位错帧问题
   - 分析 ESP32P4 端缓冲区切换逻辑
   - 检查 WebSocket 消息队列
   - 添加更详细的调试日志

2. 进一步优化帧率
   - 降低分辨率到 640x480
   - 增加帧缓冲区到 6 个
   - 提高任务优先级

3. 完善监控和日志
   - 添加性能监控
   - 完善错误日志
   - 添加告警机制

---

## 快速参考

### 常用命令速查表

| 操作 | 命令 |
|------|------|
| 启动所有服务 | `docker-compose up -d` |
| 启动指定服务 | `docker-compose up -d main-client face-detector` |
| 停止所有服务 | `docker-compose down` |
| 重启服务 | `docker-compose restart main-client` |
| 查看日志 | `docker-compose logs -f main-client` |
| 进入容器 | `docker exec -it main-client bash` |
| 查看状态 | `docker-compose ps` |
| 重新构建 | `docker-compose build --no-cache` |
| 清理资源 | `docker system prune -a` |

### 端口映射表

| 服务 | 容器端口 | 宿主机端口 | 用途 |
|------|---------|-----------|------|
| main-client | 9000 | 9000 | Web界面 |
| head-pose-detector | 9001 | 9001 | 头部姿态检测 |
| gesture-detector | 9002 | 9002 | 手势识别 |
| focus-detector | 9003 | 9003 | 注意力检测 |
| emotion-detector | 9004 | 9004 | 情绪检测 |
| fatigue-detector | 9005 | 9005 | 疲劳检测 |
| face-detector | 9006 | 9006 | 人脸检测 |

---

## 算法容器操作手册

### 算法服务概述

| 算法 | 容器名 | 端口 | 模型/方法 | 功能描述 |
|------|--------|------|-----------|----------|
| face_detector | face-detector | 9006 | 1MB-Slim ONNX | 人脸检测 + 5点关键点 |
| head_pose | head-pose-detector | 9001 | WHENet + MediaPipe | 头部姿态估计 (yaw/pitch/roll) |
| gesture | gesture-detector | 9002 | handpose_x ONNX | 手势识别 (0-5) |
| focus | focus-detector | 9003 | YOLO + MediaPipe | 专注度检测 |
| emotion | emotion-detector | 9004 | EmotiEffLib ONNX | 情绪识别 (8类) |
| fatigue | fatigue-detector | 9005 | dlib EAR/MAR | 疲劳检测 (眨眼/打哈欠) |

### 算法 API 端点

每个算法服务提供两个端点：

| 端点 | 返回内容 | 用途 |
|------|----------|------|
| `/detect` | result, processing_time | 仅返回检测结果 |
| `/detect_visualize` | result, visualized_image, processing_time | 返回结果 + 可视化图片 |
| `/health` | status, algorithm, version, method | 健康检查 |

### 启用/禁用算法

修改 `main-client/main.py` 中的 `enabled_algorithms` 配置：

```python
self.enabled_algorithms = {
    'head_pose': False,      # 头部姿态
    'gesture': True,         # 手势识别
    'focus': True,           # 专注度
    'emotion': True,         # 情绪识别
    'fatigue': True,         # 疲劳检测
    'face_detector': True    # 人脸检测
}
```

修改后重启 main-client：
```bash
docker compose -f "d:\Internship\new\docker\docker-compose.yml" restart main-client
```

### 单独启动算法容器

```bash
# 启动人脸检测
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d face-detector

# 启动手势识别
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d gesture-detector

# 启动专注度检测
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d focus-detector

# 启动情绪识别
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d emotion-detector

# 启动疲劳检测
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d fatigue-detector

# 启动头部姿态
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d head-pose-detector
```

### 停止算法容器

```bash
# 停止单个算法
docker compose -f "d:\Internship\new\docker\docker-compose.yml" stop face-detector

# 停止多个算法
docker compose -f "d:\Internship\new\docker\docker-compose.yml" stop gesture-detector focus-detector

# 停止所有算法
docker compose -f "d:\Internship\new\docker\docker-compose.yml" stop gesture-detector focus-detector emotion-detector fatigue-detector face-detector head-pose-detector
```

### 算法返回数据格式

#### face_detector
```json
{
  "success": true,
  "result": {
    "face_count": 1,
    "faces": [{
      "bbox": [x1, y1, x2, y2],
      "confidence": 0.95,
      "landmarks": {
        "left_eye": [x, y],
        "right_eye": [x, y],
        "nose": [x, y],
        "left_mouth": [x, y],
        "right_mouth": [x, y]
      },
      "landmarks_pixel": {...},
      "image_size": {"width": 800, "height": 640}
    }]
  },
  "visualized_image": "base64...",
  "processing_time": 0.05
}
```

#### head_pose
```json
{
  "success": true,
  "result": {
    "face_x": 15.5,
    "face_y": -10.2,
    "yaw": 1.55,
    "pitch": -1.02,
    "roll": 0.5,
    "method": "WHENet"
  },
  "visualized_image": "base64...",
  "processing_time": 0.08
}
```

#### gesture
```json
{
  "success": true,
  "result": {
    "type": "1",
    "confidence": 0.85,
    "keypoints_count": 21,
    "keypoints": [[x1, y1], [x2, y2], ...]
  },
  "visualized_image": "base64...",
  "processing_time": 0.03
}
```

#### focus
```json
{
  "success": true,
  "result": {
    "rating": 100,
    "label": "focused",
    "state_code": 0,
    "detections": [],
    "head_yaw": 1.5,
    "calibrating": false,
    "yolo_boxes": []
  },
  "visualized_image": "base64...",
  "processing_time": 0.12
}
```

#### emotion
```json
{
  "success": true,
  "result": {
    "type": "happy",
    "confidence": 0.92,
    "rating": 50,
    "scores": {"happy": 0.92, "neutral": 0.05, ...},
    "face_bbox": [x1, y1, x2, y2]
  },
  "visualized_image": "base64...",
  "processing_time": 0.04
}
```

#### fatigue
```json
{
  "success": true,
  "result": {
    "rating": 0.1,
    "ear": 0.25,
    "mar": 0.3,
    "har": 0.1,
    "blink_count": 5,
    "yawn_count": 0,
    "nod_count": 0,
    "face_detected": true,
    "landmarks": [[x1, y1], ...]
  },
  "visualized_image": "base64...",
  "processing_time": 0.06
}
```

---

## 常见问题排查

### 算法画面不显示

**检查步骤：**
1. 确认算法容器已启动：`docker ps`
2. 检查算法健康状态：查看 main-client 日志
3. 确认算法在 enabled_algorithms 中启用
4. 检查算法返回的 `has_visualized_image=True`

**常见原因：**
- 算法容器未启动
- 算法未在配置中启用
- 算法模型加载失败（检查容器日志）

### ESP32P4 连接超时

**检查步骤：**
1. 确认 ESP32P4 已通电
2. 确认 ESP32P4 已连接 WiFi
3. 从主机 ping ESP32P4 IP：`ping 192.168.55.143`
4. 检查 docker-compose.yml 中的 ESP32P4_IP 配置

### Focus 算法超时

**原因：** Focus 算法使用 YOLO 模型，首次启动需要加载模型（约 10-30 秒）

**解决方案：** 等待模型加载完成，或提前启动 focus-detector 容器

---

## 开发调试

### 修改代码后生效

由于 main-client 使用了 volume mount，修改代码后只需重启容器：

```bash
docker compose -f "d:\Internship\new\docker\docker-compose.yml" restart main-client
```

### 修改环境变量后生效

环境变量修改后需要重新创建容器：

```bash
docker compose -f "d:\Internship\new\docker\docker-compose.yml" up -d --force-recreate main-client
```

### 查看算法容器日志

```bash
# 查看 face-detector 日志
docker logs face-detector --tail 50

# 查看 gesture-detector 日志
docker logs gesture-detector --tail 50

# 实时跟踪日志
docker logs -f main-client
```

### 测试算法 API

```bash
# 健康检查
curl http://localhost:9006/health

# 测试人脸检测（需要 base64 图片）
curl -X POST http://localhost:9006/detect_visualize \
  -H "Content-Type: application/json" \
  -d '{"image": "<base64_image_data>"}'
```

---

更新时间: 2026-04-27
