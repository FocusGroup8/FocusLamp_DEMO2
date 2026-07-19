## 目标

在 ESP32-P4+C5 模块上实现基于 WebSocket 的摄像头视频流传输与 MCP（Model Context Protocol）控制信息双向通信，PC 端通过纯 HTML5 浏览器实时显示摄像头画面并下发控制指令。

**承接上一步**：文档 `01~06` 完成了 MIPI DSI 显示、I2S 音频、OV5647 摄像头、系统监控等基础能力的方案设计与验证。本文档定义网络传输层与控制协议层的需求规格，为后续实施提供基线。

**任务范围**：

* 做：WebSocket 服务器搭建、摄像头 JPEG 流传输、MCP 工具实现、PC 前端页面

* 不做：屏幕回显摄像头数据（已废弃）、TLS/认证（开发阶段暂不启用）、音频传输

**当前状态**：方案已确认，待实施。分支 `feature/websocket-camera-stream` 已创建。

***

## 阶段流程

### 步骤1：硬件基础确认

#### 1.1 硬件清单

| 模块       | 规格                                                  | 备注                                  |
| -------- | --------------------------------------------------- | ----------------------------------- |
| 主控       | ESP32-P4 (CONFIG\_IDF\_TARGET="esp32p4")            | P4 为应用主机                            |
| WiFi 从机  | ESP32-C5                                            | 通过 ESP-Hosted SDIO 通信，由 C5 间接实现网络功能 |
| 模组型号     | WT01P4C5-S1                                         | 与 wifi\_test 项目完全相同                 |
| 摄像头      | OV5647 MIPI-CSI 2-lane, RAW8 800x640\@50fps         | 已配置                                 |
| JPEG 编码器 | ESP32-P4 硬件 JPEG, 质量 80, YUV422, 200KB 缓冲           | 已配置                                 |
| 显示屏      | KD034WXFID001 3.4" 480x480 MIPI DSI (ST7701S+GT911) | 已配置                                 |
| PSRAM    | 已启用 (CONFIG\_SPIRAM=y)                              | 已配置                                 |

#### 1.2 关键约束

* **不能直接修改 sdkconfig.defaults**：必须先通过 `idf_component.yml` 注册依赖，由用户执行命令下载相关组件，再指导用户在 menuconfig 中配置，由用户自行保存至 sdkconfig.defaults

* **硬件与 wifi\_test 完全相同**：可全量复用 wifi\_test 的网络组件架构

***

### 步骤2：协议架构设计

#### 2.1 传输层选型决策

| 协议方案               | 延迟       | 吞吐          | 功耗    | 可靠性      | 决策结果          |
| ------------------ | -------- | ----------- | ----- | -------- | ------------- |
| ESP-NOW            | 1-18ms   | \~1Mbps     | \~8mA | 无连接易丢包   | 不采用（负载不足传输视频） |
| WiFi TCP/WebSocket | 80-300ms | 20-30MBit/s | >40mA | TCP 可靠   | **采用**        |
| HTTP MJPEG         | \~500ms  | 中           | >40mA | HTTP 短连接 | 不采用（延迟高、开销大）  |
| BLE                | >10s RTT | 低           | 最低    | -        | 不采用（带宽不足）     |

**最终方案**：WebSocket 统一方案

* 摄像头流：MJPEG 二进制帧

* MCP 控制：JSON-RPC 2.0 文本帧

* 双向通信，单协议栈，PC 浏览器原生支持

#### 2.2 WebSocket 服务器配置

| 参数  | 值         | 说明                |
| --- | --------- | ----------------- |
| 端口  | 80        | HTTP 默认端口，防火墙友好   |
| 路径1 | `/camera` | 二进制 JPEG 视频流通道    |
| 路径2 | `/mcp`    | JSON-RPC 2.0 控制通道 |
| TLS | 不启用       | 开发阶段              |
| 认证  | 不启用       | 开发阶段              |
| 心跳  | 30 秒 Ping | 防止连接超时            |
| 重连  | 自动指数退避    | 初始 5s，最大 60s      |

#### 2.3 MCP 协议规格

| 项     | 值                                           |
| ----- | ------------------------------------------- |
| 协议版本  | 2025-11-25                                  |
| 消息格式  | JSON-RPC 2.0                                |
| 数据类型  | boolean / integer / float / string          |
| 传输层   | WebSocket 文本帧                               |
| 标准 操作 | initialize / tools.list / tools.call / ping |

***

### 步骤3：组件复用策略

#### 3.1 复用清单

全量复用 wifi\_test 项目的以下组件（复制源码至当前项目 components/ 目录）：

| 组件                 | 源路径                                       | 目标路径                                     | 功能                              |
| ------------------ | ----------------------------------------- | ---------------------------------------- | ------------------------------- |
| wifi\_manager      | `wifi_test/components/wifi_manager/`      | `mipi_dsi/components/wifi_manager/`      | ESP-Hosted SDIO + C5 从机 WiFi 管理 |
| websocket\_manager | `wifi_test/components/websocket_manager/` | `mipi_dsi/components/websocket_manager/` | WebSocket 客户端+服务器双模式            |
| task\_manager      | `wifi_test/components/task_manager/`      | `mipi_dsi/components/task_manager/`      | FreeRTOS 任务统一管理                 |

#### 3.2 适配调整

* WebSocket 服务器需扩展支持**多路径分离**（/camera 与 /mcp），wifi\_test 原实现为单路径 `/ws`

* wifi\_manager 配置需通过 menuconfig 让用户配置 SSID/密码

* target 维持 `esp32p4`（wifi\_test 也是 esp32p4，无需修改）

#### 3.3 依赖注册流程

```
1. 编辑 main/idf_component.yml 添加依赖
2. 用户执行 idf.py reconfigure 下载组件
3. 指导用户 menuconfig 配置 WiFi SSID/密码、WebSocket 端口
4. 用户通过 idf.py save-defconfig 保存到 sdkconfig.defaults
```

***

### 步骤4：MCP 工具清单

#### 4.1 摄像头控制类（4个）

| 工具名                  | 参数                       | 返回                | 说明           |
| -------------------- | ------------------------ | ----------------- | ------------ |
| `camera.start`       | 无                        | `{"ok":true}`     | 启动摄像头采集与推流   |
| `camera.stop`        | 无                        | `{"ok":true}`     | 停止摄像头采集与推流   |
| `camera.set_quality` | `{"quality":int(1-100)}` | `{"quality":int}` | 调整 JPEG 编码质量 |
| `camera.set_fps`     | `{"fps":int(1-30)}`      | `{"fps":int}`     | 调整目标帧率       |

#### 4.2 显示控制类（4个）

| 工具名                      | 参数                     | 返回                | 说明        |
| ------------------------ | ---------------------- | ----------------- | --------- |
| `display.on`             | 无                      | `{"ok":true}`     | 开启显示屏     |
| `display.off`            | 无                      | `{"ok":true}`     | 关闭显示屏     |
| `display.set_brightness` | `{"level":int(0-100)}` | `{"level":int}`   | 设置背光亮度    |
| `display.show_camera`    | `{"enable":bool}`      | `{"enable":bool}` | 屏幕预览摄像头开关 |

#### 4.3 JSON-RPC 消息示例

**请求**（PC → ESP32-P4）：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "camera.set_quality",
    "arguments": {"quality": 60}
  }
}
```

**响应**（ESP32-P4 → PC）：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{"type": "text", "text": "{\"quality\":60}"}],
    "isError": false
  }
}
```

***

### 步骤5：性能指标

#### 5.1 视频流性能

| 指标      | 目标值             | 测试方法            |
| ------- | --------------- | --------------- |
| 分辨率     | 800x640         | 摄像头硬件能力         |
| 帧率      | 10-15 fps       | 浏览器接收统计         |
| JPEG 质量 | 80（默认，可调 1-100） | 文件大小校验          |
| 端到端延迟   | <300ms          | 浏览器时间戳 vs 采集时间戳 |
| 带宽需求    | 2-5 Mbps        | 路由器流量统计         |

#### 5.2 MCP 控制性能

| 指标      | 目标值    | 测试方法            |
| ------- | ------ | --------------- |
| 指令延迟    | <50ms  | JSON-RPC id 时戳差 |
| 工具执行成功率 | >99%   | 计数统计            |
| 并发客户端   | 支持 2-3 | 多浏览器标签页         |

#### 5.3 系统资源

| 指标     | 限制        | 监控方式                          |
| ------ | --------- | ----------------------------- |
| 堆内存    | 剩余 >200KB | esp\_get\_free\_heap\_size()  |
| PSRAM  | 剩余 >2MB   | heap\_caps\_get\_info()       |
| CPU 占用 | <80%      | vTaskGetRunTimeStats()        |
| 任务栈水位  | >512 字节   | uxTaskGetStackHighWaterMark() |

***

### 步骤6：PC 前端规格

#### 6.1 技术选型

* 纯 HTML5 + JavaScript + WebSocket API

* 无需后端服务，浏览器直接打开本地 HTML 文件

* 兼容 Chrome / Edge / Firefox

#### 6.2 功能清单

| 功能       | 实现方式                                                    |
| -------- | ------------------------------------------------------- |
| 摄像头画面显示  | WebSocket 接收二进制帧 → Blob → URL.createObjectURL → img.src |
| MCP 指令下发 | WebSocket 发送 JSON-RPC 文本帧                               |
| 参数调节面板   | HTML 表单 → JSON-RPC tools/call                           |
| 连接状态指示   | WebSocket onopen/onclose 事件                             |
| 日志显示     | 控制台 console.log + 页面日志区                                 |

#### 6.3 用户界面布局

| 区域 | 左侧（约 60% 宽度）       | 右侧（约 40% 宽度）                             |
| -- | ------------------ | ---------------------------------------- |
| 顶部 | 标题栏：ESP32-P4 摄像头监控 | -                                        |
| 中部 | 视频显示区（img 元素）      | 摄像头控制：\[启动] \[停止] / 质量: 滑块80 / 帧率: 滑块15  |
| 中下 | 视频显示区续             | 显示控制：\[开屏] \[关屏] / 亮度: 滑块100 / □ 屏幕预览摄像头 |
| 底部 | 日志区（跨两列）           | 日志区续                                     |

> **确认闸门**：以上需求规格已经用户确认完整准确，将作为后续实施的基线。测试计划详见 `08-测试计划.md`。

***

## 全局约束

* 所有提问、确认、信息展示必须通过交互工具进行，禁止纯文字沟通

* Agent 仅提供技术指导和流程控制，不直接执行 idf.py 编译/烧录/monitor 等需用户物理操作的动作

* 所有资源引用必须使用绝对路径

* 每个阶段必须获得用户执行前确认和执行后验证

* 禁止使用 ASCII 制图符号绘制框图，统一使用 Markdown 表格或列表结构

* 禁止直接修改 sdkconfig.defaults，必须通过 idf\_component.yml 注册 → 用户 menuconfig → 用户保存

