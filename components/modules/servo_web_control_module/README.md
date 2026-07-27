# 舵机 Web 控制模块集成与测试文档

---

**文档版本**: v1.0.0  
**创建日期**: 2026-05-19  
**作者**: AI Assistant  
**状态**: 编写中  

---

## 文档状态追踪

| 章节 | 状态 | 完成日期 | 审查人 |
|------|------|----------|--------|
| 一、概述与架构设计 | [x] 已完成 | 2026-05-19 | 待审查 |
| 二、组件设计与接口定义 | [x] 已完成 | 2026-05-19 | 待审查 |
| 三、实现步骤与代码说明 | [x] 已完成 | 2026-05-19 | 待审查 |
| 四、测试验证与联调指南 | [x] 已完成 | 2026-05-19 | 待审查 |
| 五、扩展指南与注意事项 | [x] 已完成 | 2026-05-19 | 待审查 |

---

## 目录

- [一、概述与架构设计](#一概述与架构设计)
  - [1.1 功能概述](#11-功能概述)
  - [1.2 技术方案](#12-技术方案)
  - [1.3 系统架构](#13-系统架构)
  - [1.4 依赖组件](#14-依赖组件)
  - [1.5 执行要求](#15-执行要求)
- [二、组件设计与接口定义](#二组件设计与接口定义)
  - [2.1 模块概述](#21-模块概述)
  - [2.2 目录结构](#22-目录结构)
  - [2.3 主接口设计](#23-主接口设计)
  - [2.4 类型定义](#24-类型定义)
  - [2.5 WebSocket 通信协议](#25-websocket-通信协议)
  - [2.6 状态回包设计](#26-状态回包设计)
- [三、实现步骤与代码说明](#三实现步骤与代码说明)
  - [3.1 阶段一：环境准备](#31-阶段一环境准备)
  - [3.2 阶段二：组件开发](#32-阶段二组件开发)
  - [3.3 阶段三：主工程集成](#33-阶段三主工程集成)
  - [3.4 阶段四：前端联调](#34-阶段四前端联调)
- [四、测试验证与联调指南](#四测试验证与联调指南)
  - [4.1 单元验证](#41-单元验证)
  - [4.2 集成测试](#42-集成测试)
  - [4.3 Web 联调测试](#43-web-联调测试)
  - [4.4 串口调试命令](#44-串口调试命令)
  - [4.5 验收标准](#45-验收标准)
  - [4.6 问题排查指南](#46-问题排查指南)
- [五、扩展指南与注意事项](#五扩展指南与注意事项)
  - [5.1 扩展建议](#51-扩展建议)
  - [5.2 注意事项](#52-注意事项)
  - [5.3 常见问题](#53-常见问题)
  - [5.4 版本历史](#54-版本历史)

---

## 一、概述与架构设计

> **状态**: [>] 进行中

### 1.1 功能概述

#### 1.1.1 功能目标

`servo_web_control_module` 用于为舵机系统提供基于 WebSocket 的远程控制能力，支持浏览器页面、串口 JSON、以及上层业务模块对 EM3/LX 舵机进行直接控制和状态查询。

本模块的目标包括：

1. 为 Web 前端提供统一 JSON 指令入口。
2. 为 EM3 和 LX 舵机提供单体与群组动作控制。
3. 提供舵机当前位置状态回传。
4. 支持与 `servo_control_module` 协同，实现录制、回放、切槽等动作管理能力。
5. 支持通过 HTTP/WebSocket 对接主工程或独立测试工程。

#### 1.1.2 核心特性

| 特性 | 描述 |
|------|------|
| **WebSocket 控制** | 浏览器通过 `ws://IP:PORT/ws` 向 ESP32 发送 JSON 指令 |
| **单舵机控制** | 支持 `em3_move`、`lx_move` 单体动作 |
| **群组动作控制** | 支持 `lx_group` 一次控制多个 LX 舵机 |
| **状态查询** | 支持 `get_status` / `servo_status` 查询舵机位置 |
| **录制与播放** | 可扩展支持 `servo_record`、`servo_play`、`servo_stop`、`servo_slot` |
| **串口透传** | 可选启用串口 JSON 控制桥接 |
| **模块化架构** | 按独立组件方式组织，便于在主工程或测试工程复用 |

#### 1.1.3 当前验证范围

当前首期验证重点如下：

| 范围 | 内容 | 当前状态 |
|------|------|----------|
| WebSocket 建链 | 浏览器与 ESP32 建立 WebSocket 连接 | 已验证 |
| 舵机直接控制 | 前端拖动滑块发送 `em3_move` / `lx_move` | 已验证 |
| 位置读取 | 点击“刷新状态”后返回舵机位置 | 已验证，需处理无效值 |
| 录制回放控制 | `servo_record / servo_play / servo_stop / servo_slot` | 正在接入 |
| 与主工程融合 | 与 `ws_server + ws_command_handler` 协同 | 进行中 |

---

### 1.2 技术方案

#### 1.2.1 技术选型

| 技术组件 | 选型方案 | 说明 |
|----------|----------|------|
| **网络协议** | WebSocket | 浏览器与 ESP32 实时双向通信 |
| **数据格式** | JSON | 指令清晰、调试方便 |
| **HTTP 服务** | `esp_http_server` | 提供 WebSocket URI |
| **舵机底层驱动** | `servo_driver` | 提供 EM3/LX 读写接口 |
| **动作录制回放** | `servo_control_module` | 提供记录、播放、切槽能力 |
| **前端展示** | `console.html + app.js` | 浏览器调试页面 |
| **状态上报** | 模块内部 reporter | 统一封装状态 JSON 回包 |

#### 1.2.2 工作流程

```text
浏览器页面
   │
   │ JSON 指令（WebSocket）
   ▼
servo_web_http_server / ws_server
   │
   ▼
指令解析器
servo_web_command_parser / ws_command_parser
   │
   ▼
指令执行器
servo_web_command_executor / ws_command_executor
   │
   ├── servo_driver                → 直接控制 EM3 / LX 舵机
   └── servo_control_module        → 录制 / 播放 / 切槽
   │
   ▼
状态上报器
servo_web_status_reporter / ws_status_reporter
   │
   ▼
浏览器收到 servo_status / arm_status / system_status_update
```

#### 1.2.3 数据流说明

1. 前端通过 HTTP 页面发起 WebSocket 连接。
2. ESP32 接收 JSON 文本帧。
3. 解析器根据 `type` 字段识别命令类型。
4. 执行器调用底层模块：
   - `servo_em3_move()`
   - `servo_lx_move()`
   - `servo_lx_move_group()`
   - `servo_control_start_recording()`
   - `servo_control_start_playback()`
5. 状态上报器读取舵机位置和控制状态。
6. 有效数据打包为 `servo_status` 回发前端。
7. 前端收到后更新滑块值、录制状态、槽位和帧数显示。

---

### 1.3 系统架构

#### 1.3.1 组件架构图

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                              应用层 (App)                                │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                      Web 前端 console.html / app.js             │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                              协议层 (Protocol)                           │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ servo_web_control_module / ws_command_handler                  │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐ │   │
│  │  │ JSON解析器   │  │ 命令执行器   │  │ 状态上报器           │ │   │
│  │  └──────────────┘  └──────────────┘  └──────────────────────┘ │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                              模块层 (Module)                             │
│  ┌──────────────┐  ┌────────────────────┐  ┌────────────────────┐      │
│  │ servo_driver │  │ servo_control_module│  │ ws_server / httpd  │      │
│  └──────────────┘  └────────────────────┘  └────────────────────┘      │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                              硬件层 (Hardware)                           │
│  ┌──────────────┐  ┌──────────────────────────────────────────────┐    │
│  │ EM3 Servo    │  │ LX Servo ID: 1 / 2 / 3 / 5                   │    │
│  └──────────────┘  └──────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

#### 1.3.2 组件职责说明

| 组件 | 层级 | 职责 |
|------|------|------|
| `servo_web_control_module` | Protocol | 独立 Web 控制模块，封装解析、执行、状态回传 |
| `ws_server` | Protocol | 主工程 WebSocket 服务入口 |
| `ws_command_handler` | Protocol | 主工程协议处理链 |
| `servo_driver` | Module | EM3/LX 舵机底层控制与读位置 |
| `servo_control_module` | Module | 录制、回放、切槽、状态管理 |
| `console.html` | App | 浏览器控制界面 |
| `app.js` | App | 前端事件绑定、WebSocket 收发、UI 更新 |

#### 1.3.3 依赖关系

```text
servo_web_control_module
    ├── servo_driver
    ├── servo_control_module
    ├── cjson
    └── esp_http_server

主工程 ws_command_handler
    ├── ws_server
    ├── servo_driver
    └── servo_control_module
```

---

### 1.4 依赖组件

#### 1.4.1 核心依赖

| 组件 | 来源 | 说明 |
|------|------|------|
| `servo_driver` | components | 舵机底层驱动 |
| `servo_control_module` | components | 动作录制与回放控制 |
| `esp_http_server` | ESP-IDF | WebSocket HTTP 服务 |
| `cjson` | ESP-IDF | JSON 解析与生成 |

#### 1.4.2 ESP-IDF 依赖

| 组件 | 说明 |
|------|------|
| `freertos` | 任务调度与延时 |
| `esp_http_server` | WebSocket URI 注册与帧处理 |
| `nvs_flash` | 若后续持久化配置可使用 |
| `json/cjson` | JSON 收发 |
| `log` | 串口调试日志 |

#### 1.4.3 配套工程依赖

| 路径 | 作用 |
|------|------|
| `components/servo_web_control_module` | 独立舵机 Web 模块 |
| `components/ws_server` | 主工程 WebSocket 服务器 |
| `components/ws_command_handler` | 主工程命令处理器 |
| `main/app_tasks.c` | 舵机任务初始化入口 |
| `Laptop/console.html` | 前端页面 |
| `Laptop/app.js` | 前端控制逻辑 |

---

### 1.5 执行要求

> **重要**: 本模块的调试和测试必须按阶段执行，否则容易把前端、网络、协议、驱动、硬件问题混在一起。

#### 1.5.1 分阶段执行

1. 先验证舵机驱动初始化成功。
2. 再验证 WebSocket 单连接稳定。
3. 再验证 `get_status` 是否能正确回包。
4. 再验证 `em3_move / lx_move` 直控动作。
5. 最后验证 `servo_record / servo_play / servo_stop / servo_slot`。

#### 1.5.2 用户操作指导

由于本文档面向 Chat Mode 调试，以下命令需要用户自行执行：

| 操作 | 命令 | 说明 |
|------|------|------|
| 编译项目 | `idf.py build` | 编译主工程 |
| 烧录固件 | `idf.py -p COMx flash` | 烧录到设备 |
| 查看日志 | `idf.py -p COMx monitor` | 串口观察启动与联调日志 |
| 前端本地服务 | `node .\server.js` | 启动静态页面服务器 |

#### 1.5.3 验证方式

每个阶段完成后，至少执行以下验证：

1. **编译验证**: `idf.py build` 通过。
2. **启动验证**: 串口日志出现模块初始化成功。
3. **协议验证**: 浏览器和 ESP32 间能正常收发 JSON。
4. **硬件验证**: 舵机实际转动或位置能读取。
5. **异常验证**: 非法值不污染前端 UI。

---

## 二、组件设计与接口定义

> **状态**: [>] 进行中

### 2.1 模块概述

`servo_web_control_module` 负责把 Web 请求转换为底层舵机与录制控制动作，并将状态统一打包回传给前端。

它包含五个核心文件：

| 文件 | 作用 |
|------|------|
| `servo_web_control_module.c` | 生命周期管理、启动/停止控制 |
| `servo_web_http_server.c` | WebSocket HTTP 服务封装 |
| `servo_web_command_parser.c` | JSON 解析器 |
| `servo_web_command_executor.c` | 指令执行器 |
| `servo_web_status_reporter.c` | 状态回包生成器 |

---

### 2.2 目录结构

```text
components/servo_web_control_module/
├── include/
│   ├── servo_web_control_module.h
│   ├── servo_web_control_module_config.h
│   └── servo_web_control_module_types.h
├── src/
│   ├── servo_web_control_module.c
│   ├── servo_web_http_server.c
│   ├── servo_web_command_parser.c
│   ├── servo_web_command_executor.c
│   └── servo_web_status_reporter.c
├── CMakeLists.txt
├── Kconfig.projbuild
├── README.md
└── servo_web_control_integration_guide.md
```

---

### 2.3 主接口设计

#### 2.3.1 模块生命周期接口

```c
esp_err_t servo_web_control_module_init(const servo_web_control_config_t *config);
esp_err_t servo_web_control_module_deinit(void);
esp_err_t servo_web_control_module_start(void);
esp_err_t servo_web_control_module_stop(void);
servo_web_control_state_t servo_web_control_module_get_state(void);
```

#### 2.3.2 生命周期说明

| 接口 | 说明 |
|------|------|
| `init()` | 初始化配置与状态，不直接启动 Web 服务 |
| `start()` | 启动 HTTP/WebSocket 服务、可选串口桥、状态上报任务 |
| `stop()` | 停止 Web 服务和内部任务 |
| `deinit()` | 释放模块状态，回到未初始化 |
| `get_state()` | 查询当前状态 |

#### 2.3.3 模块状态定义

| 状态 | 说明 |
|------|------|
| `SERVO_WEB_CONTROL_STATE_UNINIT` | 未初始化 |
| `SERVO_WEB_CONTROL_STATE_IDLE` | 已初始化但未运行 |
| `SERVO_WEB_CONTROL_STATE_RUNNING` | Web 控制服务运行中 |
| `SERVO_WEB_CONTROL_STATE_ERROR` | 错误状态 |

---

### 2.4 类型定义

#### 2.4.1 命令类型

当前建议支持的命令类型：

| 命令类型 | 说明 |
|----------|------|
| `em3_move` | EM3 单舵机移动 |
| `lx_move` | LX 单舵机移动 |
| `lx_group` | LX 群组动作 |
| `get_status` | 获取舵机状态 |
| `servo_status` | 兼容查询状态别名 |
| `servo_record` | 开始/停止录制 |
| `servo_play` | 播放动作 |
| `servo_stop` | 停止播放或录制 |
| `servo_slot` | 切换录制槽位 |

#### 2.4.2 参数结构设计

```c
typedef struct {
    uint8_t id;
    uint16_t position;
    uint16_t duration_ms;
} servo_web_move_params_t;
```

#### 2.4.3 Web 命令包结构

```c
typedef struct {
    servo_web_cmd_type_t type;
    uint8_t slot;
    union {
        servo_web_move_params_t single;
        struct {
            uint16_t duration_ms;
            uint8_t count;
            servo_web_move_params_t params[6];
        } group;
    } data;
} servo_web_command_t;
```

---

### 2.5 WebSocket 通信协议

#### 2.5.1 EM3 单舵机控制

```json
{"type":"em3_move","id":4,"pos":2000,"time":500}
```

字段说明：

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `em3_move` |
| `id` | number | 舵机 ID，默认 4 |
| `pos` | number | 目标位置 |
| `time` | number | 当前协议字段，EM3 侧可能被映射为速度参数 |

#### 2.5.2 LX 单舵机控制

```json
{"type":"lx_move","id":1,"pos":500,"time":500}
```

#### 2.5.3 LX 群组控制

```json
{
  "type":"lx_group",
  "time":500,
  "servos":[
    {"id":1,"pos":500},
    {"id":2,"pos":650},
    {"id":3,"pos":420}
  ]
}
```

#### 2.5.4 状态查询

```json
{"type":"get_status"}
```

或

```json
{"type":"servo_status"}
```

#### 2.5.5 录制/回放相关

开始/停止录制切换：

```json
{"type":"servo_record"}
```

开始播放：

```json
{"type":"servo_play"}
```

停止播放或停止录制：

```json
{"type":"servo_stop"}
```

切换槽位：

```json
{"type":"servo_slot","slot":1}
```

---

### 2.6 状态回包设计

#### 2.6.1 舵机状态回包

```json
{
  "type":"servo_status",
  "data":{
    "em3_4":2000,
    "lx_1":500,
    "lx_2":500,
    "lx_3":500,
    "lx_5":500
  }
}
```

#### 2.6.2 带控制状态的扩展回包

```json
{
  "type":"servo_status",
  "data":{
    "em3_4":2000,
    "lx_1":500,
    "lx_2":500,
    "lx_3":500,
    "lx_5":500,
    "control":{
      "state":0,
      "slot":0,
      "frame_count":0,
      "slot0_frames":0,
      "slot1_frames":0,
      "slot2_frames":0
    }
  }
}
```

#### 2.6.3 `control.state` 映射建议

| 数值 | 含义 |
|------|------|
| `0` | 空闲 |
| `1` | 录制中 |
| `2` | 已有数据 |
| `3` | 播放中 |

#### 2.6.4 无效值处理策略

为了避免前端出现 `-32768` 等无效值，建议：

1. 首次读取失败时不立即回前端。
2. 延迟 500ms 再补读一次。
3. 第二次仍无效时，本次不回包。
4. 前端收到 `< 0` 的值直接丢弃，保留上一次有效值。

---

## 三、实现步骤与代码说明

> **状态**: [>] 进行中

### 3.1 阶段一：环境准备

> **节点状态**: [ ] 未开始  
> **预计时间**: 20 分钟

#### 3.1.1 节点说明

本阶段确认当前工程具备以下基础能力：

- 舵机驱动已启用
- 舵机任务可启动
- Wi-Fi 与 WebSocket 可正常工作
- 前端能通过 HTTP 页面访问

#### 3.1.2 执行步骤

**步骤1：确认配置项**

重点检查以下配置：

| 配置项 | 预期 |
|--------|------|
| `CONFIG_PROJECT_ENABLE_SERVO` | `y` |
| `CONFIG_PROJECT_APP_ENABLE_SERVO_TASK` | `y` |
| `CONFIG_PROJECT_ENABLE_SERVO_WEB_CONTROL` | `y` |
| `CONFIG_PROJECT_APP_AUTO_START_TASKS` | 根据主工程策略正确配置 |

**步骤2：编译验证**

```powershell
cd 'E:\focus\lamp\Laptop'
idf.py build
```

**步骤3：烧录并查看启动日志**

```powershell
idf.py -p COM5 flash monitor
```

#### 3.1.3 验证方式

| 验证项 | 预期日志 |
|--------|----------|
| 舵机任务启动 | `Starting servo control task...` |
| EM3 初始化成功 | `EM3 servo initialized successfully` |
| LX 初始化成功 | `LX servo initialized successfully` |
| WebSocket 启动 | `WebSocket server started successfully` |

---

### 3.2 阶段二：组件开发

> **节点状态**: [x] 已开始  
> **预计时间**: 1.5 ~ 2 小时

#### 3.2.1 文件级职责

| 文件 | 关键职责 |
|------|----------|
| `servo_web_command_parser.c` | `type` 到枚举的映射 |
| `servo_web_command_executor.c` | 底层执行和状态补读 |
| `servo_web_status_reporter.c` | 舵机位置与录制状态回包 |
| `servo_web_http_server.c` | WebSocket 接入点 |
| `servo_web_control_module.c` | 生命周期、任务管理 |

#### 3.2.2 解析器要求

解析器至少要正确支持：

- `em3_move`
- `lx_move`
- `lx_group`
- `get_status`
- `servo_status`
- `servo_record`
- `servo_play`
- `servo_stop`
- `servo_slot`

#### 3.2.3 执行器要求

执行器至少要正确调用：

- `servo_em3_move()`
- `servo_lx_move()`
- `servo_lx_move_group()`
- `servo_control_start_recording()`
- `servo_control_stop_recording()`
- `servo_control_start_playback()`
- `servo_control_stop_playback()`
- `servo_control_switch_slot()`

#### 3.2.4 状态回包要求

状态回包要满足：

1. 返回有效舵机位置。
2. 无效值不直接回前端。
3. 可选带 `control` 状态对象。
4. JSON 简洁，避免冗余字段。

---

### 3.3 阶段三：主工程集成

> **节点状态**: [x] 已开始  
> **预计时间**: 1 小时

#### 3.3.1 两种集成路径

| 路径 | 说明 | 适用场景 |
|------|------|----------|
| 独立 `servo_web_control_module` | 组件自带 HTTP/WebSocket | 测试工程、独立模块验证 |
| 主工程 `ws_server + ws_command_handler` | 复用主工程 WebSocket 通道 | 当前主工程联调 |

#### 3.3.2 当前主工程实际链路

当前主工程实际运行的是：

```text
main.c
 └─ ws_server
     └─ ws_command_handler
         └─ ws_command_parser / ws_command_executor
```

因此如果前端已经接入 `servo_record / servo_play / servo_stop / servo_slot`，主工程的 `ws_command_handler` 也必须同步支持这些命令。

#### 3.3.3 单客户端连接问题

当前 `ws_server` 曾出现以下问题：

```text
WebSocket handshake completed
Another client already connected, rejecting
```

建议策略：

1. 新连接自动替换旧连接。
2. 不直接拒绝新页面。
3. 前端尽量只保留一个页面实例。

---

### 3.4 阶段四：前端联调

> **节点状态**: [x] 已开始  
> **预计时间**: 1 小时

#### 3.4.1 启动前端页面

由于前端必须通过 HTTP 打开，不能使用 `file://`，推荐：

```powershell
cd 'E:\focus\lamp\Laptop\Laptop'
node .\server.js
```

浏览器访问：

```text
http://127.0.0.1:5173/console.html
```

#### 3.4.2 前端基础功能

当前前端至少应支持：

| 功能 | 说明 |
|------|------|
| 滑块控制 | 直接发送 `em3_move` / `lx_move` |
| 刷新状态 | 发送 `get_status` |
| 状态显示 | 更新舵机当前位置 |
| 录制按钮 | 发送 `servo_record` |
| 播放按钮 | 发送 `servo_play` |
| 停止按钮 | 发送 `servo_stop` |
| 槽位选择 | 发送 `servo_slot` |

#### 3.4.3 前端日志判断

浏览器开发者工具中重点关注：

| 日志 | 含义 |
|------|------|
| `[ESP32P4 WS] 已连接到 ESP32P4` | 舵机控制链路已连通 |
| `发送指令到 ESP32:` | 前端已发出 JSON |
| `[ESP32P4 WS] 收到消息:` | 后端回包已返回 |

---

## 四、测试验证与联调指南

> **状态**: [>] 进行中

### 4.1 单元验证

#### 4.1.1 测试目标

验证 `servo_web_control_module` 各内部环节能独立工作：

- 解析器可识别所有协议
- 执行器能调用底层模块
- 状态回包格式正确
- 生命周期接口行为正确

#### 4.1.2 单元验证项

| 测试项 | 测试内容 | 预期结果 |
|--------|----------|----------|
| 模块初始化 | `servo_web_control_module_init()` | 返回 `ESP_OK` |
| 模块启动 | `servo_web_control_module_start()` | HTTP 服务启动成功 |
| 状态解析 | `servo_web_parse_command()` | 正确解析 `type` |
| 动作执行 | `servo_web_execute_command()` | 调用到底层接口 |
| 状态回包 | `servo_web_report_status()` | 返回 `servo_status` JSON |

---

### 4.2 集成测试

#### 4.2.1 测试环境要求

| 要求项 | 说明 |
|--------|------|
| 硬件 | ESP32-P4、EM3 舵机、LX 舵机、驱动板 |
| 网络 | 电脑与 ESP32 在同一网段 |
| 前端 | 本地静态服务器已启动 |
| 固件 | 主工程或测试工程已烧录支持 Web 控制的版本 |

#### 4.2.2 启动日志检查

应至少看到：

```text
I (...) main: WebSocket server started successfully on port 8080
I (...) ws_cmd_handler: WebSocket command handler initialized successfully
I (...) app_tasks: Starting servo control task...
I (...) servo_driver: LX servo initialized successfully
I (...) servo_driver: EM3 servo initialized successfully
```

---

### 4.3 Web 联调测试

#### 4.3.1 场景1：WebSocket 连接

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 启动前端页面 | 页面正常打开 |
| 2 | 打开浏览器控制台 | 无 `file://` 跨域错误 |
| 3 | 等待 WebSocket 连接 | 出现 `[ESP32P4 WS] 已连接到 ESP32P4` |
| 4 | 查看串口 | 出现 `WebSocket handshake completed` |

#### 4.3.2 场景2：刷新舵机状态

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 点击“刷新状态” | 发送 `{"type":"get_status"}` |
| 2 | 查看串口 | 收到 `Got packet with message: {"type":"get_status"}` |
| 3 | 后端读位置 | 返回有效 `servo_status` |
| 4 | 前端显示 | 滑块值与数字更新 |

#### 4.3.3 场景3：EM3 舵机控制

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 拖动 EM3 滑块 | 发送 `em3_move` |
| 2 | 串口打印 | `EM3 Move: ID=4 ...` |
| 3 | 舵机动作 | EM3 转动 |
| 4 | 状态回读 | 位置更新到前端 |

#### 4.3.4 场景4：LX 舵机控制

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 拖动任意 LX 滑块 | 发送 `lx_move` |
| 2 | 串口打印 | `LX Move: ID=x ...` |
| 3 | 舵机动作 | 目标 LX 转动 |
| 4 | 状态回读 | 前端位置更新 |

#### 4.3.5 场景5：录制与播放

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 点击“开始录制” | 发送 `servo_record` |
| 2 | 拖动多个舵机 | 录制帧数增长 |
| 3 | 点击“停止” | 录制停止 |
| 4 | 点击“播放动作” | 发送 `servo_play` |
| 5 | 舵机重放 | 执行刚才记录的动作 |
| 6 | 切换槽位 | `servo_slot` 生效 |

---

### 4.4 串口调试命令

#### 4.4.1 现有命令说明

| 命令 | 说明 |
|------|------|
| `servo_record` | 开始/停止录制 |
| `servo_play` | 播放动作 |
| `servo_stop` | 停止播放 |
| `servo_slot` | 切换槽位 |
| `servo_status` | 查看控制模块状态 |
| `servo_pos` | 查看实时舵机位置 |

#### 4.4.2 推荐排查顺序

1. `servo_pos`
2. `servo_status`
3. Web 页面点击“刷新状态”
4. Web 页面拖滑块
5. Web 页面点“录制/播放/停止”

#### 4.4.3 `servo_pos` 结果判断

| 输出 | 含义 |
|------|------|
| 正常位置值 | 舵机读位置成功 |
| `-32768` | 驱动未初始化或读取失败 |
| `servo not initialized` | 舵机任务未启动或驱动未初始化 |

---

### 4.5 验收标准

#### 4.5.1 功能验收标准

| 验收项 | 标准 |
|--------|------|
| WebSocket 建链 | 浏览器连接稳定，无频繁拒绝 |
| 刷新状态 | 点击按钮后可返回一次有效状态 |
| EM3 控制 | 拖动滑块可控制 EM3 |
| LX 控制 | 拖动滑块可控制 LX |
| 录制动作 | 可录制多帧数据 |
| 播放动作 | 可回放录制动作 |
| 切换槽位 | 槽位切换生效 |

#### 4.5.2 稳定性验收标准

| 验收项 | 标准 |
|--------|------|
| 单页面控制 | 反复刷新页面不导致永久占线 |
| 状态刷新 | 不大量回传无效负数 |
| 异常舵机 | 某个舵机异常不应导致系统崩溃 |
| 长时间运行 | 连续运行 30 分钟无崩溃 |

---

### 4.6 问题排查指南

#### 4.6.1 常见问题一：前端按钮失效

可能原因：

1. `$$` 选择器定义错误。
2. `$(".servo-slider").forEach(...)` 写错，应为 `$$(".servo-slider").forEach(...)`。
3. 页面通过 `file://` 打开导致运行环境异常。

#### 4.6.2 常见问题二：WebSocket 无法连接

典型日志：

```text
Another client already connected, rejecting
```

解决方案：

1. 只保留一个浏览器页面。
2. 修改服务器策略为“新连接替换旧连接”。
3. 重启 ESP32 清理旧连接。

#### 4.6.3 常见问题三：出现 `-32768`

可能原因：

1. 驱动未初始化。
2. 舵机无电。
3. 总线读失败。
4. ID 配置不对。
5. 返回过早，舵机尚未完成动作。

解决方案：

1. 先查 `servo_pos`。
2. 后端延时 500ms 补读一次。
3. 前端丢弃负数。
4. 检查供电、接线、ID、UART。

#### 4.6.4 常见问题四：`servo_record` 不生效

典型日志：

```text
Unsupported command type: 0
```

原因：

- 主工程实际跑的是 `ws_command_handler`，但解析器未注册 `servo_record`。

解决方案：

1. 在主工程 `ws_command_handler` 类型枚举中新增 `SERVO_RECORD / PLAY / STOP / SLOT`。
2. 在解析器里增加 `type` 字符串映射。
3. 在执行器中调用 `servo_control_module`。

#### 4.6.5 常见问题五：终端一直刷日志

原因可能包括：

1. `idf.py monitor` 本身实时输出。
2. 状态上报任务周期推送。
3. 前端自动重连频繁触发。
4. WebSocket 单连接拒绝新客户端。

解决方案：

1. 关闭不必要的周期上报。
2. 降低日志级别。
3. 暂时禁用 Docker WS 自动重连。
4. 解决旧连接占线问题。

---

## 五、扩展指南与注意事项

> **状态**: [>] 进行中

### 5.1 扩展建议

#### 5.1.1 协议扩展方向

后续可扩展：

| 能力 | 说明 |
|------|------|
| `servo_delete_slot` | 删除某槽位动作 |
| `servo_rename_slot` | 为槽位命名 |
| `servo_export` | 导出动作帧 |
| `servo_import` | 导入动作帧 |
| `servo_safe_home` | 回安全初始位 |
| `servo_group_status` | 只查询指定舵机集合 |

#### 5.1.2 前端体验优化

可继续增加：

1. 录制中按钮高亮。
2. 播放中禁止重复点击。
3. 空槽位禁止播放。
4. 录制帧数实时显示。
5. 当前有效位置缓存与回填。

---

### 5.2 注意事项

#### 5.2.1 架构注意事项

| 注意项 | 说明 |
|--------|------|
| 主工程链路优先 | 当前主工程实际用 `ws_server + ws_command_handler` |
| 独立模块与主链路并存 | `servo_web_control_module` 可作为独立验证模块 |
| 依赖声明必须完整 | `servo_driver`、`servo_control_module` 必须在 CMake 中声明 |
| 类型命名避免冲突 | 不要重复定义 `servo_control_params_t` |

#### 5.2.2 协议注意事项

| 注意项 | 说明 |
|--------|------|
| `time` 字段语义不完全一致 | LX 更接近时间，EM3 可能被映射为速度 |
| 读取位置有延迟 | 动作后不能立即读，建议延时后补读 |
| 无效值不能直传前端 | 必须在后端或前端做过滤 |

#### 5.2.3 联调注意事项

| 注意项 | 说明 |
|--------|------|
| 页面必须 HTTP 打开 | 不能使用 `file://` |
| 只保留一个控制页 | 避免 WebSocket 占线 |
| 先看开机日志 | 初始化问题不能只看后续 `get_status` |
| 先验证驱动再验证 Web | 否则容易误判为前端问题 |

---

### 5.3 常见问题

#### 5.3.1 编译问题

**Q1: `servo_driver.h: No such file or directory`**

A: 在组件 `CMakeLists.txt` 里补上：

- `PRIV_REQUIRES servo_driver`

**Q2: `conflicting types for 'servo_control_params_t'`**

A: 避免在 `servo_web_control_module` 里自定义同名结构体，改成独立命名，如：

- `servo_web_move_params_t`

**Q3: `Unsupported command type: 0`**

A: 说明解析器没有识别对应 `type`，需要同步更新：

- 枚举
- 字符串映射
- 执行器分支

#### 5.3.2 运行问题

**Q1: 前端刷新没结果**

A: 可能是：

1. 后端要求“全部舵机都有效才回包”。
2. 某个舵机无效导致整包超时。
3. 建议改成“首次失败，等 500ms，再补读一次”。

**Q2: EM3 能动但位置读不到**

A: 检查：

1. `servo_em3_read_pos()` 是否返回正常。
2. EM3 是否支持当前位置查询。
3. 读位置时机是否过早。

**Q3: LX 读值偶发失败**

A: 检查：

1. 总线稳定性。
2. 供电是否充足。
3. 舵机 ID 是否重复。
4. 是否连续高频查询导致冲突。

---

### 5.4 版本历史

| 版本 | 日期 | 变更说明 |
|------|------|----------|
| v1.0.0 | 2026-05-19 | 初版，整理 `servo_web_control_module` 架构、协议、集成与测试流程 |

---

**文档结束**

---

## 附录

### A. 参考文档

- [voice_control_integration_guide.md](file:///e:/focus/lamp/Laptop/docs/guides/voice_control_integration_guide.md)
- [README.md](file:///e:/focus/lamp/Laptop/components/servo_web_control_module/README.md)

### B. 相关组件

- [servo_web_control_module](file:///e:/focus/lamp/Laptop/components/servo_web_control_module)
- [servo_control_module](file:///e:/focus/lamp/Laptop/components/servo_control_module)
- [servo_driver](file:///e:/focus/lamp/Laptop/components/servo_driver)
- [ws_command_handler](file:///e:/focus/lamp/Laptop/components/ws_command_handler)
- [ws_server](file:///e:/focus/lamp/Laptop/components/ws_server)

### C. 推荐联调入口

- 前端页面：[console.html](file:///e:/focus/lamp/Laptop/Laptop/console.html)
- 前端脚本：[app.js](file:///e:/focus/lamp/Laptop/Laptop/app.js)
- 主工程入口：[main.c](file:///e:/focus/lamp/Laptop/main/main.c)