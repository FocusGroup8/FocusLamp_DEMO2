# FocusLamp 新架构文档

## 一、整体架构图

```
+--------------------------------------------------------------------+
|                         Application Layer (apps)                     |
|  +-------------+ +----------+ +---------------+ +----------------+  |
|  | lighting_app| |focus_app | |companion_app  | | arm_action_app |  |
|  +-------------+ +----------+ +---------------+ +----------------+  |
|  +----------------+ +-----------------+ +-------------+              |
|  | music_rhythm_app| |  game_app       | | voice_app   |            |
|  +----------------+ +-----------------+ +-------------+              |
+--------------------------------------------------------------------+
        |                     |                     |
        v                     v                     v
+--------------------------------------------------------------------+
|                         main (Init + Task Mgmt)                      |
|  +-----------+ +------------+ +---------------+ +----------------+  |
|  | app_init  | | app_tasks  | | app_state     | | event_handler  |  |
|  +-----------+ +------------+ +---------------+ +----------------+  |
+--------------------------------------------------------------------+
        |                     |                     |
        v                     v                     v
+--------------------------------------------------------------------+
|                          Service Layer (services)                    |
|  +-----------+ +----------+ +-----------+ +---------------+          |
|  | power_svc | | led_svc  | | sensor_svc| | touch_svc     |          |
|  +-----------+ +----------+ +-----------+ +---------------+          |
|  +----------+ +----------+ +-----------+ +-----------+               |
|  | lcd_svc  | |audio_svc | | servo_svc | | comm_svc |               |
|  +----------+ +----------+ +-----------+ +-----------+               |
|  +----------+                                                       |
|  | arm_svc  |                                                       |
|  +----------+                                                       |
+--------------------------------------------------------------------+
        |                     |                     |
        v                     v                     v
+--------------------------------------------------------------------+
|                          Driver Layer (drivers)                      |
|  +-----------+ +----------+ +-----------+ +---------------+          |
|  |power_drv  | | led_drv  | |ligh_sen_dr| | touch_drv     |          |
|  +-----------+ +----------+ +-----------+ +---------------+          |
|  +----------+ +----------+ +-----------+ +-----------+               |
|  | lcd_drv  | |audio_drv | |servo_drv  | | radar_drv |              |
|  +----------+ +----------+ +-----------+ +-----------+               |
|  +----------+                                                       |
|  |uart2_drv |                                                       |
|  +----------+                                                       |
+--------------------------------------------------------------------+
        |                     |                     |
        v                     v                     v
+--------------------------------------------------------------------+
|                          BSP Layer (bsp)                             |
|  +----------+ +---------+ +---------+ +---------+ +--------+        |
|  | bsp_gpio | |bsp_uart | |bsp_spi  | |bsp_i2s  | |bsp_adc |        |
|  +----------+ +---------+ +---------+ +---------+ +--------+        |
|  +----------+                                                       |
|  |bsp_power |                                                       |
|  +----------+                                                       |
+--------------------------------------------------------------------+
        |                     |                     |
        v                     v                     v
+--------------------------------------------------------------------+
|                        Hardware (ESP32-S3)                           |
|  +--------+ +---------+ +--------+ +-------+ +--------+             |
|  | WS2812 | | TEMT6000| |TTP223  | | LCD   | | I2S    |             |
|  +--------+ +---------+ +--------+ +-------+ +--------+             |
|  +--------+ +---------+ +--------+                                  |
|  | Servo1 | | Servo2  | | Radar  |                                  |
|  +--------+ +---------+ +--------+                                  |
+--------------------------------------------------------------------+

+-------------------+     +-------------------+
|  Event Bus (event_bus)   | Protocol (protocol) |
|  publish/subscribe       | UART frames, cmds   |
+-------------------+     +-------------------+
```

## 二、各层职责说明

### 1. main 层（入口层）
- **app_main()**: ESP-IDF 标准入口，调用 `app_init()` + `app_tasks_create()`
- **app_init**: 系统初始化编排，按顺序执行 NVS → 事件循环 → BSP → 事件总线 → 驱动 → 服务
- **app_tasks**: 创建所有 FreeRTOS 任务（任务表驱动，使用 `xTaskCreatePinnedToCore`）
- **app_state**: 应用状态机，管理 11 种状态（INIT/IDLE/LIGHTING/FOCUS/COMPANION/ARM_ACTION/MUSIC_RHYTHM/GAME/VOICE/SLEEP/ERROR）
- **app_event_handler**: 全局事件处理器注册，处理系统级事件、电源管理、触摸手势默认行为、看门狗、低电量等

### 2. common 层（公共组件）
- 数据类型定义（`data_type.h`）：颜色、触摸事件、系统状态、应用模式枚举
- 错误码定义（`error_code.h`）：模块化错误码，每个模块有独立 Base
- 系统配置（`system_config.h`）：任务栈大小、优先级、超时、外设参数
- 引脚配置（`pin_config.h`）：所有 GPIO 映射集中管理

### 3. BSP 层（板级支持包）
- 硬件外设总线初始化（GPIO/UART/SPI/I2S/ADC）
- 电源控制管脚初始化
- 所有 BSP 函数以 `bsp_` 前缀命名

### 4. drivers 层（硬件驱动层）
- 直接操作硬件外设（RMT/SPI/I2C/UART/I2S/ADC）
- 不包含业务逻辑，只提供底层硬件抽象
- 每个驱动有独立的 init/set/get 接口
- 驱动列表：power_driver、led_driver、touch_driver、lcd_driver、audio_driver、servo_driver、radar_driver、uart2_driver（light_sensor_driver 已随光感模块移除）

### 5. services 层（服务层）
- 封装驱动层，提供面向功能的高级接口
- 订阅事件总线，通过事件驱动响应
- 服务间可相互调用
- 服务列表：power_service、led_service、sensor_service、touch_service、lcd_service、audio_service、servo_service、comm_service、arm_service

### 6. apps 层（应用层）
- 实现具体产品功能模式
- 通过事件总线与其他层通信
- 每个 app 独立开发，可插拔
- 应用列表：lighting_app、focus_app、companion_app、arm_action_app、music_rhythm_app、game_app、voice_app

### 7. protocol 层（通信协议层）
- 双板通信的协议帧定义与解析
- 命令解析器
- 状态包封装
- 被 comm_service 调用

### 8. event_bus 层（事件总线层）
- 发布/订阅模式的事件通信机制
- 解耦各模块间的直接依赖
- 支持带数据和定时戳的事件
- 所有模块通过 `event_bus_subscribe` / `event_bus_publish` 交互

## 三、数据流说明

### 3.1 典型数据流 — 触摸操作

```
触摸传感器 (TTP223) → touch_driver (GPIO读取)
  → touch_service (手势识别)
    → 发布 EV_TOUCH_A_DOUBLE_CLICK 事件
      → app_event_handler (模式切换)
        → app_state_manager_set_state(APP_STATE_FOCUS)
          → 发布 EV_APP_MODE_CHANGED 事件
            → led_service → led_driver 切换灯效
            → lcd_service → lcd_driver 更新显示
            → focus_app 启动
```

### 3.2 典型数据流 — 雷达感应（环境光感应已随光感模块移除）

```
雷达模块 → radar_driver (UART解析)
  → sensor_service (数据处理)
    → 发布 EV_RADAR_* 事件
      → app_event_handler (更新 device_state：在场/心率/呼吸/HRV)
      → focus_app (在位久坐提醒，雷达为主播报源)
```

### 3.3 典型数据流 — 双板通信

```
主板 comm_service → uart2_driver (UART发送)
  → 副板接收 → uart_protocol 帧解析
    → 发布 EV_COMM_DATA_RECEIVED 事件
      → 相应的 service/app 处理
```

## 四、事件总线通信机制

事件总线是系统的"神经系统"，采用发布/订阅模式：

```
+-------------------+       +-------------------+
|  发布者 (Publisher) |       |  订阅者 (Subscriber) |
|  - touch_service   |       |  - led_service     |
|  - sensor_service  |       |  - lcd_service     |
|  - app_state       |       |  - app_event_handler|
+-------------------+       +-------------------+
         |                            |
         v                            ^
  event_bus_publish()        event_bus_subscribe()
         |                            |
         +-----------> event <--------+
                      type: event_type_t
                      data: void*
                      data_size: size_t
                      timestamp: uint32_t
```

**关键特性：**
- 事件类型使用 16 位枚举，按模块分组（0x01xx=系统, 0x02xx=电源, 0x03xx=触摸...）
- 支持同步发布与异步处理
- 每个事件携带时间戳，便于调试和时序分析

## 五、任务调度说明

### 任务优先级分配原则

| 优先级 | 任务 | 说明 |
|-------|------|------|
| 6 (最高) | watchdog_task | 系统监控，最高优先级确保能及时检测故障 |
| 5 | power_task | 电源管理，保证低电量时能及时响应 |
| 4 | comm_task, servo_task | 通信和舵机控制，需要低延迟 |
| 3 | sensor_task, touch_task, audio_task | 传感器、触摸、音频，中等优先级 |
| 2 | led_task, lcd_task | 灯效和显示，较低优先级 |
| 1 (最低) | app_task | 应用调度，最低优先级 |

### 任务间通信方式

1. **事件总线** (主要)：任务处理完工作后发布事件，其他任务/服务通过订阅响应
2. **任务通知** (ulTaskNotifyTake)：用于高效的任务间信号传递
3. **队列** (xQueue)：用于模块内的数据传递
4. **互斥锁**：保护共享资源访问

## 六、与旧项目（FocusLamp）的关系

FocusLamp_New 是在原 FocusLamp 项目基础上的重构版本，主要变化：

| 维度 | 旧项目 | 新项目 |
|------|--------|--------|
| 架构 | 单层，逻辑耦合 | 分层：main/bsp/drivers/services/apps |
| 通信 | 直接函数调用 | 事件总线 + 服务调用 |
| 驱动 | 混杂在业务代码 | 独立 driver 层，可替换 |
| 任务 | 手动管理 | 任务表驱动，统一创建 |
| 配置 | 散落各处 | 集中到 common/system_config.h |
| 扩展 | 添加功能困难 | 新增 app/service 即插即用 |