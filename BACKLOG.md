# FocusLamp_DEMO2 项目后续任务清单

> 本文档记录 FocusLamp_DEMO2 项目尚未完成的任务,按优先级排序。
> 最后更新:2026-07-28

## 高优先级任务

### 1. ~~修复 status_reporter 目标 IP 配置~~ (已完成)
- **问题**: `CONFIG_STATUS_REPORTER_TARGET_IP` 曾为 "10.132.136.143",实际 wifi_test IP 为 "10.132.136.181"
- **解决**: 已在 menuconfig 中修改 Component config -> status_reporter -> Target IP,通信已恢复
- **状态**: 已完成
- **备注**: 若后续无法通信,首要排查 IP 配置是否匹配(DHCP 可能分配不同 IP)

### 2. 完成状态上报端到端测试
- **目标**: 验证 FocusLamp 能成功向 wifi_test 上报状态数据
- **测试项**:
  - [ ] 定时上报(10 秒间隔)成功送达 wifi_test
  - [ ] 事件触发上报(LED/Display/Servo/LCD 控制)成功送达
  - [ ] JSON 数据格式正确完整
  - [ ] wifi_test 的 status_receiver 能正确解析
- **状态**: 待测试(依赖任务 1 完成)

### 3. REST API 功能验证
- **目标**: 验证所有 19 个 REST API 端点功能正常
- **测试项**:
  - [ ] 系统查询(3): /api/status, /api/system/info, /api/radar/status
  - [ ] LED 控制(6): on/off/brightness/effect/color/status
  - [ ] Display 控制(3): on/off/brightness
  - [ ] LCD 控制(4): page/next, expression, blink/enable, blink/disable
  - [ ] Servo 控制(3): position(POST/GET), home
- **状态**: 部分功能已验证,部分待测试

## 中优先级任务

### 4. 阶段 D:MCP 工具包装为 REST 端点
- **目标**: 配合 wifi_test 实现 MCP 工具的 REST 端点暴露
- **依赖**: 状态上报端到端测试完成
- **状态**: 未开始

### 5. 阶段 E:音频控制命令接口
- **目标**: 实现音频相关的控制命令接口
- **依赖**: 阶段 D 完成
- **状态**: 未开始

## 低优先级任务

### 6. WiFi 信号优化
- **问题**: RSSI -87 ~ -88 dBm,信号较弱
- **可能方案**: 调整天线位置、减小距离、检查干扰源
- **状态**: 观察中

### 7. SDIO 传输稳定性监控
- **问题**: 启动日志中出现 "Dropping packet(s) from stream" 和 "Failed to push data to rx queue" 警告
- **影响**: 可能导致少量数据包丢失,但未影响核心功能
- **状态**: 观察中

### 8. ESP-Hosted 协处理器版本一致性
- **问题**: Host FW 2.12.11,需确认与 Slave(Co-processor)版本一致性
- **状态**: 已知问题,暂无影响

## 已完成任务

- [x] 创建 status_reporter 组件(HTTP 客户端,定时+事件触发上报)
- [x] 创建 rest_api 组件(19 个 REST API 端点)
- [x] 集成 status_reporter 到 network_manager
- [x] 在 rest_api 控制处理函数中添加事件通知
- [x] 修复 ESP-Hosted sdio_mempool_create 断言失败(启用 CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM)
- [x] 修复 REST API URI 槽位已满(max_uri_handlers 16 → 32)
- [x] 修复编译错误(组件依赖、头文件包含、枚举未定义等)
