# 知识库 - 问题与解决方案总结

> 本文档记录了项目开发过程中遇到的所有问题及其解决方案，供后续开发参考。

---

## 目录

1. [Docker 相关问题](#1-docker-相关问题)
2. [算法容器问题](#2-算法容器问题)
3. [WebSocket 通信问题](#3-websocket-通信问题)
4. [前端渲染问题](#4-前端渲染问题)
5. [ESP32P4 设备问题](#5-esp32p4-设备问题)
6. [代码架构问题](#6-代码架构问题)
7. [最佳实践](#7-最佳实践)

---

## 1. Docker 相关问题

### 1.1 Docker Desktop 未启动

**问题现象：**
```
error during connect: This error may indicate that the docker daemon is not running.
```

**原因：** Docker Desktop 应用程序未运行

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

### 1.2 Docker 镜像源 DNS 解析问题

**问题现象：**
```
ERROR [internal] load metadata for docker.io/library/python:3.9-slim
failed to solve with frontend dockerfile.v0
```

**原因：** Docker 默认镜像源访问慢或被墙

**解决方案：**
```json
// Docker Desktop 设置 -> Docker Engine 添加镜像加速器
{
  "registry-mirrors": [
    "https://docker.mirrors.ustc.edu.cn",
    "https://hub-mirror.c.163.com"
  ]
}
```

---

### 1.3 OpenCV 缺少系统库

**问题现象：**
```
ImportError: libxcb.so.1: cannot open shared object file: No such file or directory
```

**原因：** OpenCV 依赖的系统库未安装

**解决方案：** 在 Dockerfile 中添加：
```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender1 \
    libxcb1 \
    && rm -rf /var/lib/apt/lists/*
```

---

### 1.4 Windows Docker Desktop host 网络模式问题

**问题现象：** 容器启动成功，但无法通过 localhost 访问服务

**原因：** Windows Docker Desktop 不支持 host 网络模式

**解决方案：** 使用 bridge 网络模式（默认）并配置端口映射：
```yaml
services:
  main-client:
    ports:
      - "9000:9000"  # 端口映射生效
```

---

### 1.5 Docker 缓存导致代码不更新

**问题现象：** 修改代码后重新构建，但代码未更新

**原因：** Docker 使用了缓存层

**解决方案：**
```bash
# 方法1: 使用 --no-cache 强制重新构建
docker compose build --no-cache main-client

# 方法2: 使用 volume mount（开发环境推荐）
volumes:
  - ./main-client:/app

# 方法3: 重新创建容器
docker compose up -d --force-recreate main-client
```

---

### 1.6 环境变量修改不生效

**问题现象：** 修改 docker-compose.yml 中的环境变量后，容器仍使用旧值

**原因：** restart 命令不会重新加载环境变量

**解决方案：**
```bash
# 使用 up -d 重新创建容器
docker compose up -d --force-recreate main-client

# 而不是 restart
# docker compose restart main-client  # 不会更新环境变量
```

---

## 2. 算法容器问题

### 2.1 算法画面不显示

**问题现象：** 前端页面算法卡片无画面，显示 "Waiting for data..."

**原因：** main-client 调用了 `/detect` 端点，而不是 `/detect_visualize`

**解决方案：**
```python
# main-client/main.py
async def call_algorithm(self, algorithm_name: str, image_base64: str, timestamp: int):
    async with self.session.post(
        f"{url}/detect_visualize",  # 使用 visualize 端点
        json={"image": image_base64, "timestamp": timestamp},
        timeout=10
    ) as response:
        # ...
```

**关键点：**
- `/detect` - 仅返回 result，无 visualized_image
- `/detect_visualize` - 返回 result + visualized_image

---

### 2.2 ESP32P4 收不到算法结果

**问题现象：** ESP32P4 端收到的算法结果为空

**原因：** `detect_async` 中结果解析错误，算法结果在 `result` 子对象中

**错误代码：**
```python
gesture = latest.get('gesture')
if gesture and gesture.get('type'):  # type 在 result 子对象中，不在顶层
    result["gesture"] = gesture.get('type')
```

**正确代码：**
```python
gesture_resp = latest.get('gesture')
gesture_data = gesture_resp.get('result', {}) if gesture_resp else {}
if gesture_data and gesture_data.get('type'):
    result["gesture"] = gesture_data.get('type')
```

**数据结构说明：**
```json
{
  "success": true,
  "result": {
    "type": "1",
    "confidence": 0.85
  },
  "visualized_image": "base64...",
  "processing_time": 0.03
}
```

---

### 2.3 Focus 算法卡死

**问题现象：** Focus (Mantis) 算法完全无响应，前端无画面，所有请求超时。

**原因：** 单线程 Flask + 每帧调用（skip_frames=1）+ 30 FPS 请求量导致请求队列堵塞。Focus 内部使用 MediaPipe LIVE_STREAM + ONNX 推理，单帧处理时间较长，请求堆积后全部超时。

**解决方案：**
1. 在 `focus/server.py` 中添加 `processing_lock` 忙碌跳过机制：
```python
import threading
processing_lock = threading.Lock()

@app.route('/detect_visualize', methods=['POST'])
def detect_visualize():
    if not processing_lock.acquire(blocking=False):
        return jsonify({"success": False, "error": "Server busy, skipping frame"}), 503
    try:
        # 处理逻辑
    finally:
        processing_lock.release()
```

2. Flask 启动添加 `threaded=True`
3. main-client 中 Focus 超时从 3s 改为 8s
4. 503 响应静默处理：`elif response.status == 503: return None`
5. Focus skip_frames 设为 2（~15 FPS）

**关键点：** Focus 平均处理时间仅 8ms，但偶发峰值可达 306ms，skip_frames=1 时偶发延迟会导致请求积压雪崩。

---

### 2.4 Focus 算法首次启动超时

**问题现象：** Focus 算法启动后前几次请求超时

**原因：** YOLO 模型加载需要时间（约 10-30 秒）

**解决方案：**
1. 提前启动 focus-detector 容器
2. 等待模型加载完成后再启用算法
3. 增加请求超时时间

---

### 2.4 Emotion 情绪词条颜色显示为白色

**问题现象：** Emotion 卡片中 happiness/sadness 等情绪词条颜色显示为白色，与预期颜色不匹配。

**原因：** EmotiEffLib 返回的情绪名为 "happiness"/"sadness"/"anger"，但颜色映射表 `emotion_colors` 和 `EMOTION_TO_RATING` 使用的是 "happy"/"sad"/"angry"，键名不匹配导致查找失败，回退为默认白色。

**解决方案：** 在 `emotion/server.py` 中同步修改键名为 EmotiEffLib 实际返回的名称：
```python
EMOTION_TO_RATING = {
    "anger": -0.5, "happiness": 0.5, "sadness": -0.5,
    "disgust": -0.6, "fear": -0.7, "surprise": 0.2, "contempt": -0.3, "neutral": 0.0
}
emotion_colors = {
    "happiness": (0, 255, 0),    # 绿色
    "sadness": (255, 100, 0),    # 橙色
    "anger": (0, 0, 255),        # 红色
    "disgust": (0, 180, 0),      # 深绿
    "contempt": (0, 140, 200),   # 青色
    ...
}
```

**关键点：** 使用第三方库时，必须确认其返回的数据格式和字段名，不能想当然地假设。

---

### 2.5 画面方向不一致（前置摄像头镜像）

**问题现象：** Fatigue 和 Gesture Mantis 的可视化画面方向与其他算法相反。

**原因：** 其他算法在处理时添加了 `cv2.flip(image, 1)` 进行前置摄像头镜像校正，但 Fatigue 和 Gesture Mantis 缺少此处理。

**解决方案：** 在 `detect()` 和可视化函数中添加水平翻转：
```python
# detect 函数中
image = cv2.flip(image, 1)

# 可视化函数中
vis_image = cv2.flip(image.copy(), 1)
```

**关键点：** 新增算法时需检查是否需要镜像校正，保持与其他算法一致。

---

### 2.6 可视化文字无背景导致难以阅读

**问题现象：** 算法可视化画面上的调试文字在亮色背景上难以阅读。

**解决方案：** 添加 `draw_text_with_bg` 函数，在文字下方绘制黑色矩形背景：
```python
def draw_text_with_bg(img, text, pos, font, scale, color, thickness):
    (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, thickness)
    cv2.rectangle(img, (pos[0] - 2, pos[1] - text_h - 2),
                 (pos[0] + text_w + 2, pos[1] + baseline + 2), (0, 0, 0), -1)
    cv2.putText(img, text, pos, font, scale, color, thickness)
```

将所有 `cv2.putText` 替换为 `draw_text_with_bg`。

---

### 2.7 Focus Lamp 控制消息禁用算法

**问题现象：** Face Detector、Gesture Mantis 等算法在 Focus Lamp 客户端连接后被自动禁用。

**原因：** Focus Lamp 客户端发送控制消息覆盖了 `enabled_algorithms` 配置。

**解决方案：** 添加 `forced_algorithms` 集合，在控制消息处理时强制启用指定算法：
```python
self.forced_algorithms = {'focus', 'emotion', 'fatigue', 'face_detector'}

# 控制消息处理中
for algo in self.forced_algorithms:
    self.enabled_algorithms[algo] = True
```

---

### 2.8 算法调用频率与处理速度不匹配

**问题现象：** 算法大量超时（TIMEOUT），前端无画面。

**原因：** skip_frames 设置的调用频率超过算法实际处理能力。例如 Emotion 平均耗时 213ms（最大FPS仅4.7），但 skip_frames=3 导致理论调用 10 FPS，请求积压后全部超时。

**解决方案：** 基于实测 processing_time 数据调整 skip_frames：

| 算法 | 平均耗时 | 最大FPS | skip_frames | 实际调用FPS |
|------|---------|---------|------------|------------|
| Focus | 8ms | ~120 | 2 | ~15 FPS |
| Face Detector | 34ms | ~30 | 1 | ~30 FPS |
| Fatigue | 44ms | ~22 | 3 | ~10 FPS |
| Emotion | 213ms | ~4.7 | 6 | ~5 FPS |

**关键点：** skip_frames 设置的调用频率不应超过算法最大FPS的50%，留出余量防止积压。可通过 main-client 日志中的 `processing_time` 字段监控实际处理速度。

---

### 2.9 head_pose 数据未发送给 ESP32P4

**问题现象：** ESP32P4 收不到头部姿态数据

**原因：** `detect_async` 中遗漏了 head_pose 的结果提取

**解决方案：** 添加 head_pose 数据提取：
```python
head_pose_resp = latest.get('head_pose')
head_pose_data = head_pose_resp.get('result', {}) if head_pose_resp else {}
if head_pose_data and head_pose_data.get('face_x') is not None:
    result["head_pose"] = {
        "face_x": head_pose_data.get('face_x'),
        "face_y": head_pose_data.get('face_y'),
        "yaw": head_pose_data.get('yaw'),
        "pitch": head_pose_data.get('pitch'),
        "roll": head_pose_data.get('roll')
    }
```

---

### 2.10 gesture_mantis 卡片无画面

**问题现象：** gesture-mantis-detector 服务运行正常，但 Dashboard 中卡片无画面。

**原因：** `forced_algorithms` 集合在初始化时未应用到 `enabled_algorithms`，导致 gesture_mantis 默认为 False。

**解决方案：** 在 `main-client/main.py` 初始化时立即应用 forced_algorithms：
```python
self.forced_algorithms = {
    'focus', 'emotion', 'fatigue', 'face_detector', 'gesture_mantis'
}

# 初始化时立即应用
for algo in self.forced_algorithms:
    self.enabled_algorithms[algo] = True
```

**关键点：** `forced_algorithms` 原本只在收到控制消息时应用，初始化时也需要应用。

---

### 2.11 Camera 帧率配置

**问题现象：** 本地摄像头帧率只有 15 FPS。

**原因：** docker-compose.yml 中配置了 `FPS=15`。

**解决方案：** 修改 docker-compose.yml 中的帧率配置：
```yaml
camera-capture:
  environment:
    - FPS=30  # 修改为 30 或更高
```

**注意：** 本地运行的 camera-capture 需要重启才能生效：
```powershell
$env:FPS=30; python d:\Docker\docker\camera-capture\main.py
```

---

### 2.12 人脸关键点坐标系统

**问题现象：** 不清楚鼻尖坐标的原点位置。

**说明：** face-detector 返回的 `landmarks` 坐标系：
- **原点**：图像中心 (width/2, height/2)
- **x 轴**：向右为正
- **y 轴**：向上为正

**示例：** `[19, -65]` 表示鼻尖在图像中心右侧 19 像素，下方 65 像素。

**对比：** `landmarks_pixel` 使用像素坐标，原点为左上角。

---

## 3. WebSocket 通信问题

### 3.1 WebSocket 连接超时

**问题现象：**
```
[ERROR] Connection error: timed out during opening handshake
```

**原因：**
1. ESP32P4 设备未通电
2. ESP32P4 未连接 WiFi
3. IP 地址配置错误

**解决方案：**
1. 确认 ESP32P4 已通电
2. 确认 ESP32P4 已连接 WiFi
3. 检查 docker-compose.yml 中的 ESP32P4_IP 配置
4. 从主机 ping ESP32P4 IP 验证连通性

---

### 3.2 WebSocket 参数不支持

**问题现象：**
```
TypeError: create_connection() got an unexpected keyword argument 'read_limit'
```

**原因：** websockets 库版本不支持某些参数

**解决方案：** 移除不支持的参数：
```python
async with websockets.connect(
    self.ws_url,
    ping_interval=10,
    ping_timeout=20,
    close_timeout=10,
    max_size=10 * 1024 * 1024
    # 不要使用 read_limit 参数
) as ws:
```

---

### 3.3 WebSocket 发送帧错误

**问题现象：**
```
WebSocket send error: [Errno 9] Bad file descriptor
```

**原因：** WebSocket 连接已断开，发送操作阻塞

**解决方案：** 使用非阻塞方式发送，添加错误处理：
```python
try:
    await asyncio.wait_for(ws.send(data), timeout=5.0)
except asyncio.TimeoutError:
    print("Send timeout")
except Exception as e:
    print(f"Send error: {e}")
```

---

## 4. 前端渲染问题

### 4.1 浏览器缓存导致代码不更新

**问题现象：** 修改前端代码后，浏览器仍显示旧版本

**解决方案：** 在 HTML 中添加缓存控制头：
```html
<meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
<meta http-equiv="Pragma" content="no-cache">
<meta http-equiv="Expires" content="0">
```

---

### 4.2 Canvas 渲染闪烁

**问题现象：** 前端画面闪烁、跳跃

**原因：** 渲染逻辑问题，旧帧未清除

**解决方案：**
1. 使用 requestAnimationFrame 进行渲染
2. 添加帧序号验证，跳过旧帧
3. 使用缓存图像对象

```javascript
let lastRenderedSeq = 0;

function displayFrameOnCanvas(imageBase64, seq) {
    if (seq !== undefined && seq <= lastRenderedSeq) {
        return;  // 跳过旧帧
    }
    // 渲染逻辑...
    lastRenderedSeq = seq;
}
```

---

## 5. ESP32P4 设备问题

### 5.1 旧帧闪现

**问题现象：** 正常播放过程中，闪烁的帧是几秒前出现过的帧

**原因：** ESP32P4 端缓冲区管理问题，发送了旧帧数据

**解决方案：**
1. 在发送前复制缓冲区数据
2. 添加帧序号和时间戳
3. 使用双缓冲机制

```c
// 发送前复制数据
uint8_t *send_buf = heap_caps_malloc(jpeg_size, MALLOC_CAP_SPIRAM);
if (send_buf != NULL) {
    memcpy(send_buf, s_jpeg_output_bufs[current_buf_index], jpeg_size);
    ret = ws_server_send_frame(send_buf, jpeg_size, HTTPD_WS_TYPE_BINARY);
    free(send_buf);
}
```

---

### 5.2 JPEG 质量不生效

**问题现象：** 修改 JPEG 压缩质量后，画面质量未改变

**原因：**
1. 代码未正确编译
2. CMakeLists.txt 配置问题

**解决方案：**
1. 执行 fullclean 清理编译缓存
2. 确认 CMakeLists.txt 包含正确的源文件
3. 重新编译并烧录

---

### 5.3 WiFi 连接问题

**问题现象：** ESP32P4 无法连接 WiFi

**原因：**
1. WiFi SSID 或密码错误
2. 使用了 5G 频段（ESP32P4 仅支持 2.4G）

**解决方案：**
1. 确认使用 2.4G 频段 WiFi
2. 检查 main.c 中的 WiFi 配置：
```c
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
```

---

## 6. 代码架构问题

### 6.1 算法结果数据结构不一致

**问题描述：** 不同算法返回的数据结构不一致，导致解析困难

**解决方案：** 统一算法返回格式：
```json
{
  "success": true,
  "result": { ... },
  "visualized_image": "base64...",
  "processing_time": 0.05
}
```

---

### 6.2 异步任务管理

**问题描述：** 多个算法同时运行时，任务管理混乱

**解决方案：**
```python
# 使用 asyncio.create_task 并发执行
for algorithm_name in enabled_list:
    asyncio.create_task(
        self.call_algorithm_and_store(algorithm_name, image_base64, timestamp)
    )
```

---

## 7. 最佳实践

### 7.1 开发环境配置

```yaml
# docker-compose.yml 开发环境推荐配置
services:
  main-client:
    volumes:
      - ./main-client:/app  # 代码热更新
    environment:
      - WEBSOCKET_PING_INTERVAL=5  # 更短的心跳间隔
      - WEBSOCKET_PING_TIMEOUT=10
```

### 7.2 调试技巧

```bash
# 查看容器日志
docker logs main-client --tail 100 -f

# 进入容器调试
docker exec -it main-client bash

# 测试算法 API
curl http://localhost:9006/health
```

### 7.3 启动顺序

```bash
# 推荐启动顺序
# 1. 启动核心服务
docker compose up -d main-client

# 2. 启动需要的算法服务
docker compose up -d face-detector gesture-detector

# 3. 等待模型加载（特别是 focus-detector）
# 4. 在前端页面点击 Start
```

### 7.4 代码修改流程

```bash
# 修改 Python 代码后
docker compose restart main-client

# 修改环境变量后
docker compose up -d --force-recreate main-client

# 修改 Dockerfile 后
docker compose build --no-cache main-client
docker compose up -d main-client
```

---

## 附录：常见错误速查表

| 错误信息 | 原因 | 解决方案 |
|---------|------|----------|
| `docker daemon is not running` | Docker Desktop 未启动 | 启动 Docker Desktop |
| `libxcb.so.1: cannot open` | OpenCV 缺少系统库 | 安装 libxcb1 等依赖 |
| `HTTP 404` | WebSocket 端点错误 | 检查 URL 路径 |
| `timed out during opening handshake` | ESP32P4 未通电/未连接 | 检查设备状态 |
| `has_visualized_image=False` | 调用了 /detect 而非 /detect_visualize | 修改 API 端点 |
| `Algorithm timeout` | 模型加载中或频率过高 | 等待启动或降低 skip_frames |
| `connection refused` | 容器未启动或端口错误 | 检查容器状态和端口映射 |
| `HTTP 503` | 算法容器忙碌，跳过帧 | 正常现象，降低 skip_frames |
| 情绪词条颜色为白色 | 键名与库返回值不匹配 | 同步修改映射表键名 |
| 画面方向不一致 | 缺少 cv2.flip 镜像校正 | 添加 cv2.flip(image, 1) |
| 算法被自动禁用 | Focus Lamp 控制消息覆盖 | 添加 forced_algorithms |

---

更新时间: 2026-05-23
