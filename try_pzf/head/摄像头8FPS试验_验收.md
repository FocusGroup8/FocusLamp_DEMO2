# head 高档 8 FPS 单变量试验（2026-10-17）

> 2026-10-18：本试验已暂停，当前源码HIGH已恢复5FPS，并采用保守画质/关闭修正。以下保留为历史试验记录，不是当前操作指南；当前步骤见《摄像头5FPS保守修正_验收.md》。

状态：只准备源码，未编译、未烧录、未更改运行参数。设备不会自动切换版本。

## 改了什么

仅把 `camera_stream.c` 的 HIGH 档 FPS 从 5 改为 8；LOW=2、MID=5。JPEG 质量表、PI 目标码率及参数、1500ms 整组发送预算、残帧立即断连、零进度预算丢帧、自适应降档与健康恢复规则全部保留。算法、Docker、controller 语音和心跳不改。

HIGH 的 PI 目标仍是 60 KiB/s，不是硬性限速。提高 FPS 仍可能增加实际流量；若超时更多，8 FPS 就不适合作为当前链路的常用档。

## 与留存版本的关系

修改前的 head/controller/Docker 源码与当时已有的烧录文件保存在：

`C:\Users\Administrator\Desktop\try_PZF\version_snapshots\2026-10-17_5fps_before_8fps`

其中 `head-source.zip` 的 HIGH 仍为 5。源码、已有 bin 和板上实际固件是三种不同证据：本次没有读取设备固件，也没有证明已有 bin 就是当前设备正在运行的版本。

## 用户决定上板后再做的对照

本批只需更新 head（灯头 COM4），无需更新 controller（基座 COM6）或重建 Docker。烧录前核对设备。此处不要求现在编译或烧录。

1. 新固件联网后，先设 5 FPS 上限，再开启摄像头，保持现有热点、VPN、算法和浏览器数量不变，连续观察 5～10 分钟。
2. 设 8 FPS 上限，再观察相同时间。上限不是强制速率：MID 仍为 5，LOW 仍为 2；满足恢复条件才可能进入 HIGH=8，网络慢时实收仍可能更低。
3. 对比每分钟的成功发送量、预算超时增量、断连次数、实际 FPS 和最长无新帧间隔。网页帧号持续增长才表示有新图，单独一个 FPS 数字不够。
4. 同时验证语音响应、灯光和机械臂控制没有明显变差。不要只以峰值达到 8 判通过。

PowerShell 示例，IP 必须使用 head 本次实际 Got IP 地址：

```powershell
$headIp = '10.87.104.143'
# 基线阶段
Invoke-RestMethod -Uri "http://$headIp/api/camera/fps" -Method Post -ContentType 'application/json' -Body '{"fps":5}'
# 单变量提速阶段
Invoke-RestMethod -Uri "http://$headIp/api/camera/fps" -Method Post -ContentType 'application/json' -Body '{"fps":8}'
```

实际 FPS 用同一时钟的时间差计算：相邻日志累计成功帧数之差 ÷ 两条日志的实际间隔秒数；同时对照 Docker 实收帧数。断连空窗计入观测时间。设备与电脑时间未同步，不能直接相减作为端到端延迟。

## 回退

试验中若出现更多超时、长停顿或控制响应变差，立即把 API 上限设回 5。该设置只用于本次运行；当前源码默认用户上限仍为 15，重启后不能假定还停在 5。

永久回退本批单变量：把 HIGH `.fps = 8` 改回 5，或从 `head-source.zip` 取回 `main/camera/camera_stream.c`。已有 bin 另行留存，是否烧回由用户决定。不要覆盖 controller 或 Docker 的后续独立改动。

本次只做源码范围核对，尚无 C 编译或实机 8 FPS 验收结论。
