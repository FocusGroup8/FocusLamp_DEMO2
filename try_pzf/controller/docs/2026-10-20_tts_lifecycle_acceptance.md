# 欢迎语生命周期修复：烧录与验收

## 当前批次：30 秒首响应诊断

本批生产代码仅将 `SPEAK_FIRST_TTS_WAIT_US` 从 5 秒改成 30 秒。上行隔离、独占周期、排队条件、SDK、网络参数和 head 全部不改；本地雷达播报方案只评估，不在本批迁移。

本批仅烧 controller／COM6。烧录重启后先不要唤醒进入连续对话，等待“Connected to server — waiting for wake word”，然后只拍一次灯头。

1. 必须先出现 `Speak cycle=... reserved` 和 `submitted`。只有 `queued/deferred` 说明尚未开始本次试验，不能按 30 秒判读。若仍在普通对话，原有长按灯头约 3 秒可真正结束会话（同时熄灭头灯）；已排队请求可能随后自动执行，不要连续追加请求。
2. 保留从 `reserved` 到 `released` 的完整日志，观察 `TTS started` 的时间。计时在两次 SDK 文本发送前进入 WAITING 时开始，不是严格从 `submitted` 日志那一刻开始。
3. START 在第 5～30 秒到达，支持原 5 秒预算过紧；30 秒仍无 START，只能说明本次延长观察未解决问题，不能判定云端完全未处理。
4. 已经收到 START 的播放不受首响应计时截断。SDK 同步发送若阻塞，整个调用仍可能超过 30 秒，因此这不是请求总时限。
5. 观察结束并完成重连后，再唤醒进行正常对话验证。普通对话导致欢迎语持续排队的问题仍待另批处理。

## 本次改动

- 灯头读取 `/api/tts/speak` 响应正文，仅 HTTP 200 且布尔 `ok:true` 才记为“已受理／已入队”，不是“已播放”。业务失败不自动重试；原网络重试参数不变。
- 基座沿用现有播报 FIFO，串行处理完整播报周期。普通对话进行中，欢迎语及同入口的文字提醒等待，不插入当前对话。
- 在发送前登记等待状态；收到有效 START 后才进入播放。原首测预算为 5 秒，本批仅将 WAITING 观察窗放宽到 30 秒；已经开始的播报不受该计时截断。
- 注入结束仅清本地麦克风暂停状态，不额外发送 listen/start；后台工作任务完成旧连接销毁、旧事件排空和新连接创建后，才释放周期。
- SDK 事件不含请求身份，本地代际号不能识别迟到的云端消息。因此播报收尾会重建实际云端连接；短暂重连是本版的明确代价。没有修改 SDK、心跳、发送超时或重连参数。
- 光感、灯效、摄像头、自适应帧率及麦克风采集实现不变。

## 已验证与未验证

- controller：完整 `idf.py build` 成功，生成 `controller/build/controller.bin`。
- head：`idf.py app` 和随后完整 `idf.py build` 均成功，生成 `head/build/mipi_dsi.bin` 与 `head/build/storage.bin`，应用分区大小检查通过。
- 本批 controller 30 项 Python 离线检查通过，包含源码接缝检查及独立策略模型；head 本批未改，沿用此前 37 项检查结果。这些检查不等同于 C／RTOS 实机测试。
- `test_speak_cycle.c` 通过 RISC-V GCC 严格编译；本机没有合适的宿主 C 编译器，未运行 C 断言。
- 两端 `sdkconfig`、`dependencies.lock` 与本次修改前备份一致；光感、音频采集和摄像头关键文件的哈希也一致。
- 未烧录、未擦除 NVS、未连接串口、未进行上板验收。首次欢迎语是否实际有声、正常语音识别和云端响应仍需实测。

灯头文件系统工具环境已修复：恢复缺失的 `pip.exe`，将原 Python 3.10 扩展替换为匹配当前 Python 3.12 的同版本 `littlefs-python==0.15.0`。完整构建和 LittleFS 打包已经通过，未修改托管组件源码或设备配置。本次功能不涉及文件系统内容，仍推荐下面的 `app-flash`，不必全量烧录或擦除。

## 烧录

在已经加载 ESP-IDF 5.5.4 环境的终端执行；若串口监视正在运行，先退出监视。

本批只更新基座 COM6：

```powershell
cd C:\Users\Administrator\Desktop\try_PZF\controller
idf.py -p COM6 app-flash monitor
```

head／COM4 本批不烧。`app-flash` 只更新应用，不更新模型或 LittleFS，不擦除配网信息。

## 按顺序验收

本批先完成上面的单变量试验，以下是原生命周期修复的回归场景，不要先做普通对话再将其排队现象误当成 30 秒观察结果。

电脑与两块板保持原网络，等待板间发现和小智连接完成。建议分别保留两端日志。

| 场景 | 操作与预期 |
| --- | --- |
| 普通对话基线 | 不拍头，唤醒后提问并连续说两句，确认能识别和回复。 |
| 欢迎语后对话 | 结束普通对话，空闲时单击灯头。观察请求受理、播报 START／STOP、后台重连；重新连接后再唤醒提问。不能持续出现“无 TTS 却 block_tts 增长”的残留暂停。 |
| 普通对话中拍头 | 正在正常对话时单击。欢迎请求等待，不抢占当前对话；正常结束会话后再处理。 |
| 连续两条请求 | 空闲时分两次触发单击，避免被识别成双击调亮度。第二条等待前一周期完整清理，不能覆盖其归属。 |
| 首音未返回 | 如现场再次出现无 START，检查首响应超时、后台清理与后续重连；恢复后再唤醒提问，不能把下一次正常 STOP 当欢迎结束。无需主动断网制造此项。 |
| 长播报 | 收到 START 后继续观察，确认 PLAYING 不受首响应计时影响；完整验证该边界需一条超过 30 秒的播报，本批尚未进行上板验证。 |
| 取消 | 播报期间使用原有结束会话操作；清理后再次唤醒，迟到 STOP 不应重新开启上行或关掉新会话。 |

基座关键日志：`Speak cycle=... reserved` → `submitted` → `TTS started` → `TTS ended` → `replacing old transport` → `released after cleanup`。

`released` 的 outcome：1=收到匹配播报结束，2=错误，3=等待 START 超时，4=取消，5=断线。即使 outcome=1，也只证明收到了云端播报结束事件，扬声器是否有声仍以现场为准。

## 留存

修改前文件与旧 ELF／BIN 位于：

`C:\Users\Administrator\Desktop\try_PZF\version_snapshots\2026-10-19_before_tts_lifecycle_fix`

当前应用 BIN SHA256：

```text
controller.bin  A3A55ADB30E7D4A43DCB6D00036C466B4E13B52B4118F06D1B9EAAD715C4660E
mipi_dsi.bin    F93144498F29E571AD1ED8FF5EDB00C1024A5DAC4B5BD2A003CF31F822E05975
```

本批 30 秒改动前的源码、测试、说明、配置与基座 ELF／BIN 另存于：

`C:\Users\Administrator\Desktop\try_PZF\version_snapshots\2026-10-20_controller_before_30s_tts_probe`

## 雷达方案复用评估

雷达久坐提醒经 `local_reminder_submit` 使用固件内嵌的 `audio/sedentary_reminder.wav`，格式为 16 kHz／单声道／PCM16。工作任务在播放前复查是否允许播报，临时暂停本地唤醒处理，经 `audio_bridge_play_local_pcm16` 输出，再恢复识别；它不经过云端文字注入。

可复用的是本地 PCM 播放与收尾方式；当前接口只绑定这一条久坐音频，没有接受任意文字或选择多条欢迎录音的能力。工程里未找到三条欢迎语的预录音频。若以后采用本地欢迎语，需要准备对应音频、扩展片段选择和欢迎语自己的触发条件；不能直接沿用雷达在位／模式条件，也不能把久坐提示音当欢迎语。本批未改这些路径。
