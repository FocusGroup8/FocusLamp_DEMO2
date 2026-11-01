# 普通模式三步任务引导 · 2026-10-31

首版实现已完成，两个 ESP32-P4 固件编译通过。未烧录、未修改云端角色配置。

## 现场修补（2026-10-31）

用户日志确认首版 controller 已刷入，但普通模式实际为 idle，被误限制为 companion；“清理桌面分3步”也未命中触发条件。
已修正两板模式判断、触发说法，并让网页文字请求在真正发送前携带三步 MCP 指令及请求令牌，避免作为普通聊天发送。
原短文本队列保持不变，长指令只临时占用 PSRAM；回声不替换令牌，失败会取消对应等待。
生产组件测试覆盖了日志中的实际原句、idle、HTTP 准备/MCP 提交、回声及超时；两板重编译通过，release 已更新。
增加 TASK_GUIDE 串口日志以区分工具注册、请求打开、三条接受、灯头预览及点击。
这份修补尚未刷入设备，云端模型真实调用与 GT911 真机流程仍未验证。日志中的音频网络超时/重连没有被本次任务功能修补解决。

## 交付

- `release/head-mipi_dsi.bin`：灯头应用，GT911 点击、中文屏幕、离线计时。
- `release/controller.bin`：基座 controller 应用，请求门控、MCP 工具及 HTTP 内容同步。
- `release/manifest.json`：本轮应用文件大小、SHA256 和应用偏移。不是全闪存镜像。
- `release/OFL-NotoSansSC.txt`：中文位图字体许可证，分发固件时保留。
- `previews/task-guide-contact-sheet.png`：生产绘制代码输出的 RGB565 主机预览，不是真机截图。
- `previews/timer-style-comparison.png`：专注模式与任务引导的计时样式对比；两者共用 `timer_ring_renderer.h`，圆环、数字尺寸/位置和配色统一。
- `../../controller/docs/task-guide-spec.md`：最终规格 v3。
- `../../controller/docs/task-guide-integration.md`：工具协议和应合并到实际智能体的角色规则。

## 已验证

- 两个固件 ESP-IDF 5.5.4 编译、分区容量检查通过。
- 本地状态机：预览不计时、180/360/540 秒边界、断网继续、点击退出、旧结果/旧令牌不重弹。
- 实际 head JSON 包装、触摸事件适配和 RGB565 画面测试通过。
- 任务引导与实际专注 renderer 在 03:00、01:30、00:01 的圆环/数字像素一致；原专注 renderer 八种状态回归通过。
- 中文字库字形、裁剪、stride、缺字策略测试通过。
- 原有模式/专注 HTTP 合同与新增三步数据合同测试通过。

复跑（仓库根目录 PowerShell）：

```powershell
./tools/tests/test_task_guide_model.ps1
./head/tests/test_cjk_font.ps1
./tools/task-guide/test_head_integration.ps1
./tools/focus-ring/test_http_contract.ps1
./tools/focus-ring/build.ps1 -Board head
./tools/focus-ring/build.ps1 -Board controller
```

## 真机待验证

两板应用更新后，在实际绑定智能体合并调用规则，结束旧会话后新建会话。
普通模式说“帮我拆解整理桌面的任务”，检查预览、点击、三分钟换步、九分钟结束及断网退出。
首版 16px 字号按真实观看距离验证；尚未证明 GT911 实际触摸灵敏度、屏幕帧率或云端模型调用稳定性。
不支持口头修改单步；等待生成默认超时 30 秒，提示只显示在屏幕。
