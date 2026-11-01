# 专注模式稳定性修补 · 2026-10-30

当前已确认基座启动标识变化且会话归零，用户未手动复位。网页超时/404发生于复位后的初始化窗口。
尚未拿到复位瞬间panic日志，不能断言栈溢出就是最终原因。

本包修正：
- HTTP任务默认4KB改为8KB；参与度上报不再嵌套完整诊断响应，而返回512字节缓冲的精简权威状态。
- /api/mode增加http_stack_free_bytes；/api/device/info增加reset_reason与uptime_ms；启动打印RESET_DIAG。
- Docker参与度名称兼容真实提供者Highly-Engaged / Engaged / Barely-engaged / Not-Engaged。
  当前电脑的该Docker修复已经部署，不必再次更新Docker。

操作：先关闭基座COM6串口监视器，再运行：

powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash-ControllerFix.ps1 -Port COM6

只烧基座应用，不写NVS/语音模型/分区表，不烧灯头。完成后网页重新连接、恢复控制、启动一次专注。
红色未参与暂停是预期；灰色无效/过期暂停与断联锁定是不同情况。

实机验收仍待完成：保持同一boot/session连续专注，不能再自动归零；查看MODE_HTTP的栈余量，发生复位时记录RESET_DIAG及panic前后日志。
仅见Server: client disconnected不能证明WiFi断开；HTTP短连接结束也会记录这行。
云端小智的空闲重连不应重置controller本地专注会话。
