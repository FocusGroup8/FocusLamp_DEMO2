---
status: active
created_at: 2026-07-10
updated_at: 2026-07-10
version: 1.0.0
---

# WebSocket功能开发需求确认文档

## 一、项目背景

基于启明云 ESP32-P4C5 开发板（ESP32-P4 Host + ESP32-C5 Co-processor），在已完成 WiFi Remote STA 通信的基础上，开发 WebSocket 通信功能。

## 二、功能需求

### 2.1 运行角色

| 需求项 | 确认结果 |
|--------|----------|
| 运行角色 | 客户端 + 服务端 |
| 客户端 | 基于 `espressif/esp_websocket_client` 托管组件（v1.7.0） |
| 服务端 | 基于 ESP-IDF 内置 `esp_http_server` 组件 |

### 2.2 数据传输

| 需求项 | 确认结果 |
|--------|----------|
| 数据类型 | 文本帧 + 二进制帧 |
| 文本帧 | JSON 命令、状态信息、配置数据 |
| 二进制帧 | 传感器原始数据、文件传输等 |

### 2.3 TLS 安全

| 需求项 | 确认结果 |
|--------|----------|
| TLS 支持 | 可配置（menuconfig 开关） |
| ws:// | 默认模式，适用于内网测试 |
| wss:// | 可选启用，需配置服务器证书 |

### 2.4 应用场景

| 需求项 | 确认结果 |
|--------|----------|
| 主要场景 | 双向实时通信 |
| 客户端场景 | 连接远程 WebSocket 服务器，实时收发数据 |
| 服务端场景 | 接受浏览器/APP 等外部客户端连接 |

## 三、技术设计

### 3.1 心跳机制

| 需求项 | 确认结果 |
|--------|----------|
| 心跳方式 | 内置 Ping/Pong 保活机制 |
| 配置参数 | keep_alive_interval, keep_alive_timeout |
| 超时处理 | 超时后触发断线事件，启动重连 |

### 3.2 重连策略

| 需求项 | 确认结果 |
|--------|----------|
| 策略类型 | 指数退避重连 |
| 初始间隔 | 1s |
| 退避因子 | 2x（1s → 2s → 4s → 8s → ...） |
| 最大间隔 | 可配置（默认 60s） |
| 最大重试 | 可配置（默认无限） |

### 3.3 服务端并发

| 需求项 | 确认结果 |
|--------|----------|
| 并发连接 | 多客户端连接 |
| 最大连接数 | 可配置（默认 4） |
| 连接管理 | 维护连接列表，支持广播/单播 |

### 3.4 数据回调

| 需求项 | 确认结果 |
|--------|----------|
| 回调模式 | 事件回调模式 |
| 事件类型 | 连接/断开/数据/错误 |

### 3.5 错误处理

| 需求项 | 确认结果 |
|--------|----------|
| 策略 | 自动重连 + 通知业务层 |
| 连接失败 | 记录日志 → 触发指数退避重连 → 超限后通知 |
| 运行时错误 | 记录日志 → 尝试恢复 → 无法恢复则终止 |

### 3.6 模块架构

| 需求项 | 确认结果 |
|--------|----------|
| 架构方式 | 双组件独立封装 |
| WiFi 组件 | `components/wifi_manager/` |
| WebSocket 组件 | `components/websocket_manager/` |
| 依赖关系 | websocket_manager 依赖 wifi_manager（WiFi 先连接才有网络） |

### 3.7 配置方式

| 需求项 | 确认结果 |
|--------|----------|
| 连接参数 | API 运行时设置（URI、端口等） |
| 运行参数 | menuconfig 配置（心跳间隔、缓冲区大小、重连参数等） |
| 功能开关 | menuconfig 控制（TLS 启用、客户端/服务端启用等） |

## 四、组件规范

### 4.1 组件结构（遵循 COMP-001~006）

```
components/{name}/
├── include/
│   ├── {name}_config.h      # 构建配置映射
│   ├── {name}_types.h       # 类型定义
│   └── {name}.h             # 公共 API
├── src/
│   └── {name}.c             # 实现
├── CMakeLists.txt           # 构建配置（含启用/禁用开关）
└── Kconfig.projbuild        # menuconfig 配置选项
```

### 4.2 配置要求

- 所有组件配置必须通过 menuconfig 工具完成
- 禁止直接修改 sdkconfig 文件
- 需指导用户查看对应组件的 Kconfig 文件获取配置选项

### 4.3 代码参考

- 必须参考本地 ESP-IDF SDK 示例代码
- SDK 路径：`C:/Software/Espressif/.espressif/v5.5.4/esp-idf`
- 客户端参考：`esp-protocols/components/esp_websocket_client/examples/`
- 服务端参考：`examples/protocols/http_server/ws_echo_server/`

## 五、开发流程

### 5.1 分阶段执行

| 阶段 | 内容 | 前置条件 |
|------|------|----------|
| 阶段 1 | WiFi 模块模块化封装与测试 | 需求确认文档通过 |
| 阶段 2 | WebSocket 模块设计与接口定义 | 阶段 1 完全通过 |
| 阶段 3 | WebSocket 功能实现与单元测试 | 阶段 2 通过 + websocket-basic 分支 |
| 阶段 4 | 集成测试与性能优化 | 阶段 3 通过 |

### 5.2 分支策略

- WiFi 封装在当前分支完成
- WebSocket 开发在 `websocket-basic` 分支进行

### 5.3 验证要求

- 每个阶段完全通过后才进入下一阶段
- 通过交互工具进行阶段确认

## 六、依赖组件

| 组件 | 来源 | 版本 | 用途 |
|------|------|------|------|
| esp_wifi_remote | ESP Component Registry | >=0.10,<2.0 | WiFi Remote 通信 |
| esp_hosted | ESP Component Registry | ~2 | ESP-Hosted SDIO 传输 |
| esp_websocket_client | ESP Component Registry | ~1.7 | WebSocket 客户端 |
| esp_http_server | ESP-IDF 内置 | - | WebSocket 服务端 |
| tcp_transport | ESP-IDF 内置 | - | WebSocket 传输层 |
