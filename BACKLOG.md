# wifi_test 项目后续任务清单

> 本文档记录 wifi_test 项目尚未完成的任务,按优先级排序。
> 最后更新:2026-07-28

## 高优先级任务

### 1. ~~修复 status_reporter 目标 IP 配置~~ (已完成)
- **问题**: FocusLamp_DEMO2 的 `CONFIG_STATUS_REPORTER_TARGET_IP` 曾为 10.132.136.143,实际 wifi_test IP 为 10.132.136.181
- **解决**: 已在 FocusLamp_DEMO2 menuconfig 中修改为目标 IP,通信已恢复
- **状态**: 已完成
- **备注**: 若后续无法通信,首要排查 IP 配置是否匹配(DHCP 可能分配不同 IP)

### 2. 完成 status_receiver 端到端测试
- **目标**: 验证 wifi_test 能正确接收并解析 FocusLamp 上报的状态数据
- **测试项**:
  - [ ] 定时上报(10 秒间隔)能被 status_receiver 接收
  - [ ] 事件触发上报(LED/Display/Servo/LCD 控制)能被接收
  - [ ] JSON 数据解析正确(led/display/lcd/servo/radar/ambient_light/system)
  - [ ] status_receiver_get_status() 能获取最新状态
- **状态**: 待测试

### 3. focuslamp_bridge 控制功能验证
- **目标**: 验证 wifi_test 通过 focuslamp_bridge 控制 FocusLamp 的功能
- **测试项**:
  - [ ] LED 控制(开/关/亮度/颜色/效果)
  - [ ] Display 控制(开/关/亮度)
  - [ ] LCD 控制(页面切换/表情/眨眼)
  - [ ] Servo 控制(位置/归位)
  - [ ] 系统信息查询
  - [ ] 雷达状态查询
- **状态**: 部分功能可用,部分功能待验证

## 中优先级任务

### 4. 阶段 D:MCP 工具包装为 REST 端点
- **目标**: 将 wifi_test 的 MCP 工具暴露为 REST API,支持 FocusLamp 反向控制
- **依赖**: focuslamp_bridge 控制功能验证完成
- **状态**: 未开始

### 5. 阶段 E:音频控制命令接口
- **目标**: 实现音频相关的控制命令接口
- **依赖**: 阶段 D 完成
- **状态**: 未开始

## 低优先级任务

### 6. WiFi 信号优化
- **问题**: RSSI -88 dBm,信号较弱
- **可能方案**: 调整天线位置、减小距离、检查干扰源
- **状态**: 观察中

### 7. SDIO 传输稳定性监控
- **问题**: FocusLamp 启动日志中出现 "Dropping packet(s) from stream" 警告
- **影响**: 可能导致少量数据包丢失,但未影响功能
- **状态**: 观察中

## 已完成任务

- [x] 创建 focuslamp_bridge 组件(HTTP 客户端,控制 FocusLamp)
- [x] 创建 status_receiver 组件(HTTP 服务端,接收 FocusLamp 状态)
- [x] 扩展 websocket_manager 提供 URI 注册接口
- [x] 集成到主程序(main/CMakeLists.txt, hello_world_main.c)
