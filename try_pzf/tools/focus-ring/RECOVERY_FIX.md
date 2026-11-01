# 专注数据链路修复 · 2026-10-31

本次现场读到：controller 会话仍在，remaining_ms=300000，pause_reason=data_stale；Docker 没有新画面，最后帧1080持续老化。摄像头开关开启不等于 Docker 收到了新帧。

本轮修复：

- 灯头：摄像头 WebSocket 每5秒 Ping，匹配的 Pong 超过15秒未返回时回收旧连接，允许 Docker 重连。仅影响摄像头连接；健康连接不会被替换。解决中断后的占位，不声称已找到首次断流的原因。
- 基座：2FPS下30样本分类间隔约15秒，原逻辑会在下一次分类前清除防抖候选。现在仅在有效新鲜样本持续到达时保留候选；分类过期期间仍暂停，下一条匹配的新分类才恢复。不提高TTL、不修改算法步长。
- Docker：一次推理请求失败不再虚构新的来源并重新绑定。失败样本仍无效，不能推动倒计时。
- 灯头 /api/status 增加摄像头连接、心跳版本、Pong年龄、超时次数，方便区分“开关开启”与“视频实际送达”。

## 操作

本机 Docker main-client 已更新。关闭两块板子的串口监视器，确认 COM4 是灯头、COM6 是基座，然后在本目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash-FocusRecovery.ps1 -HeadPort COM4 -ControllerPort COM6
```

脚本校验两份SHA256后，仅写0x10000应用；不擦NVS、不改分区表、语音模型或LittleFS，不启动监视器。两块板都是P4，芯片校验无法代替端口角色确认。

烧录后网页重新连接、恢复控制、启动摄像头。确认画面持续刷新，再重新进入专注模式。不要通过反复停止/启动摄像头来排障；旧摄像头CSI停止再启动还有已知限制。

## 预期

- 首次模型仍需要300个有效人脸样本：5FPS约60秒、2FPS约150秒，实际受有效样本比例影响。之后每30样本分类；正常状态防抖需要匹配的新分类，因此不是固定1秒就能变色。
- 高度参与/参与：绿、正常减；勉强参与：橙、半速；未参与：红、暂停。
- 无效/过期/无输入：灰、暂停；不能用旧结果强行扣时间。
- 当前照片的05:00是配置的5分钟；没有强行改成2分钟。

## 验证

灯头当前地址示例10.23.38.143，基座10.23.38.181。用网页查看画面和专注卡片是否更新；灯头GET /api/status应出现camera_heartbeat_version=1。Docker /perception的camera.connected应为true，frame_seq应推进。基座 /api/mode的pause_reason应与实际参与度一致。

已通过：实际C摄像头WebSocket编码/心跳/回收测试；实际C计时、controller_modes、HTTP契约测试；11项Docker元数据测试；61项已有Docker离线回归；两块固件构建与分区尺寸校验。

仍待烧录后的真人验收：视频持续输入、颜色变化、真实倒计时、两屏一致性和实际断线重连。编译、模拟测试不能替代此项。

本机回退固件在version_snapshots/2026-10-31_before_focus_recovery；此前的数据等待修复包保留，不要混用新旧bin与manifest。
