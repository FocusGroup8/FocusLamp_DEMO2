# 异步语音上传：范围、验证与上板验收

本页记录语音隔离/诊断批次。2026-10-19 已按用户要求恢复光感功能，语音实现保留；
叠加光感后的最新构建身份、功能范围与上板步骤见 `ambient_breathing_acceptance.md`。
下文关于“光感保持回退”及固件哈希的描述仅适用于当时批次，不代表恢复光感后的当前整机版本。

## 本批范围

只修改 controller 的上传隔离及其必要的句柄生命周期协调。audio_bridge 保持不变。
光感保持已回退状态，点头150/500ms保留；head、Docker、10秒音频发送参数、text超时、
心跳、重连退避/16秒复用宽限参数、sdkconfig、dependencies.lock均不主动调整。

这不是网络根因修复：网络持续不可写时仍然无法保证云端识别。

## 行为

- mic callback只校验、复制、非阻塞入队；不等待网络、不逐帧分配内存。
- 8个待发送槽，每槽最多1500字节；满时丢最旧待发送帧。发送任务另持独立帧副本，
  因此池内覆盖不会改写在途数据。8个待处理帧在60ms帧长下约480ms，不是总发送时限。
- 监听、TTS、本地播报在入队和发送前均检查；TTS、断线、会话关闭、重连清待发送帧。
- 代际号不取消已经进入SDK的调用。迟到结果不能关闭新代际的入口，但仍需计数、解除句柄占用。
- 音频及同步控制调用都先捕获/登记句柄；仅在真实调用返回后解除占用。
- 重连定时器只通知专用重建任务。等待旧调用退出时不持SDK/队列锁，不堵SDK事件线程。
- 15秒是应用观察一次 drain 的等待窗口，不是网络硬截止。到期保留旧句柄；重建任务继续等待，
  不强删发送任务、不在旧调用退出前新建替代实例。显式stop/deinit遇忙可返回超时而保留资源。
- 新增固定应用生命周期资源：队列池约12KiB（优先PSRAM，失败时使用可用8位堆）、
  上传任务8192字节栈、重建任务4096字节栈及RTOS控制块。stop/deinit后保留并在后续init复用，
  不逐次重建累积。初始化分配失败会报错，不回退到阻塞mic同步上传。
- `xiaozhi_manager_send_audio()`现为复制入队，ESP_OK仅表示接收，不表示已到服务器。

## 电脑端检查

`tools/test_uplink_buffer.c`直接包含实际使用的C头文件，覆盖FIFO、满队丢旧、非法长度、
源缓冲复用、在途副本独立、清队和10000次有界写入。
`tools/test_uplink_buffer.py`负责用本机C编译器编译并执行该测试，不是Python参考模型。
没有可用主机编译器时只报告未执行，不把语法检查视为运行通过。

ESP-IDF目标编译和链接只能验证构建，不替代板端并发、RAM及识别验收。

### 上一批异步隔离验证记录（2026-10-19，增加应用层诊断之前）

- ESP-IDF 5.5.4完整构建及最终增量构建均通过；目标ESP32P4，生成controller.bin。
- 最终应用大小 `0x2e25b0`，6MiB应用分区剩余52%。
- 最终ELF SHA256：`ebffc88074c44264983a824f502338cfe9c32828d3f343894675ce79e42e853d`
- 最终BIN SHA256：`497c44281699f705c5323396aa39994ee8fdc07612326847efa9681aaebf9676`
- C队列测试通过RISC-V GCC的 `-std=c11 -Wall -Wextra -Werror` 编译。
  本机Espressif Clang没有x86主机后端，故主机运行测试未执行；不记作运行通过。
- 构建后逐文件SHA256确认sdkconfig、dependencies.lock、audio_bridge、两个语音托管组件、
  LED服务、点头动作文件和manager CMakeLists均与本次修改前留存一致。
- 未烧录、未连接串口、未进行云端语音或真实网络故障验收。

本机旧 `C:/Espressif/tools/python/v5.5.4/venv` 指向已不存在的Python310，
本次改用已存在且依赖匹配的 `C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe`，
只调整构建缓存中的解释器和本次进程环境，没有安装依赖或修改全局Python/VS Code配置。

## 本批追加：仅应用层时间诊断（2026-10-19）

只在现有 `[UPLINK_DIAG]` 五秒汇总行追加三个量，不新增逐帧日志：

- `cloud_event_age_ms`：距本连接中上次进入 manager 的对应业务回调的时间。
  覆盖 TTS 状态/句子、STT 文本、LLM 表情和服务端系统命令等 chat 回调；
  不覆盖 MCP、hello、WS PING/PONG，不把本地 CHAT_ERROR、CONNECTED、DISCONNECTED、
  CHANNEL_OPENED/CLOSED 当成云端业务接收。不是全部 WebSocket 接收时间。
- `audio_rx_age_ms`：距本连接中上次进入 manager 的有效音频回调的时间。
  仅表示应用收到音频，不表示已经解码、播放或后续播放一定成功。
- `queue_dwell_ms`：最后一个开始发送的合法出队帧，在待发送队列里的驻留时间。
  不含同步 SDK 发送耗时，不是队列中最旧帧当前年龄。没有新出队时保留最后观测，
  因而发送卡住时它可能不变；必须与 `inflight_ms` 和 `drop_oldest` 增量分别看。

全部使用 `esp_timer_get_time()` 单调时钟。未观测为 `-1`，`0` 是合法的亚毫秒观测。
进入 INITIALIZED/CONNECTING/ERROR/IDLE 等非活动连接状态时清空这三个量；
不跨连接沿用。不为 TTS 暂停、关闭音频会话而伪造一次接收。
队列仍为 8 帧，溢出仍丢最旧；仅每帧追加一个随音频/代际整体复制的时间戳。
不新增过期丢弃、重试、超时或调度规则。SDK、sdkconfig、dependencies.lock、
10 秒音频发送参数、text 超时、心跳、重连规则和 audio_bridge 保持不变。

### 判读边界

- `inflight_ms` 增长且 MIC_DIAG 的 read/rx/enc 推进，支持本地采集/编码未被该发送拖停。
- 某个接收 age 降低或重新出现非负值，只说明对应回调发生过；不代表全部接收健康。
- 发送 age 和接收 age 同时增长，不能确认接收在等发送锁；正常下行静默、
  网络双向停顿也可能出现同样现象。age=0 不保证音频识别成功。
- TTS 播放包含本地解码队列，服务端可能分批、间歇或已经发完音频；播放不等于持续收流。
  原有 TTS 暂停上行规则保留，不为构造观察窗口而开启打断/双向持续发送。
- 队列只限帧数、不限年龄；480 ms 不是硬上限，不设 100 ms 等硬验收阈值。
- 确认共享锁阻塞需要 SDK 侧“RX 已准备读取且正在等锁”的直接观测，本批未增加此项。

### 实测流程

只烧基座 COM6。启动后先记录三值为 `-1`；说 focus，完整听完问候后说一句具体指令，
例如“现在几点了”，再说第二句。保留从启动到失败、断线、重连之后至少一个五秒窗口的完整日志。
重点核对 MIC_DIAG、UPLINK_DIAG、UPLINK_QUEUE、STT 原文及 transport 错误，
不只截一个年龄值。重连后在尚未有对应业务/音频回调时应重新为 `-1`。
实际用户语句产生 STT/回答才算识别成功；仅返回 focus 或问候不算成功。

本批修改前源码/固件留存于
`version_snapshots/2026-10-19_controller_before_app_diagnostics/`；
恢复需成套恢复 manager、队列头文件与主程序，按该目录 README 执行。

### 本批构建验证

- ESP-IDF 5.5.4 增量编译、链接及镜像大小检查通过；应用大小 `0x2e28a0`，6MiB 分区余量52%。
- 当前 ELF SHA256：`45f166b71070d08839f5cf9e6cab51286f2a9a9d43dc5ff3e93d9676bdbd1c2b`
- 当前 BIN SHA256：`fc2d694d2970543ac93d07b8b356415afc3931fe5f3558a8a401eedb9bba7c05`
- 扩展后的实际 C 队列测试通过 RISC-V `-std=c11 -Wall -Wextra -Werror` 编译；
  测试包含时间戳复制/覆盖、0时刻、未来/无效时刻、大于480ms和64位时间值。
  本机 Clang 无 x86 主机后端，测试运行器明确报告 No C tests were run；不记作运行通过。
- 与本批留存逐文件 SHA256 对照：两个语音 SDK 源文件、sdkconfig、dependencies.lock、
  audio_bridge、LED、点头动作和主机测试运行器均保持一致。
- 未烧录、未打开串口。尚未验证板端接收时间、重连清零和实际用户语句识别。

## COM6 上板流程（由使用者操作，本次不自动烧录）

只连接/选择基座controller的COM6；不要烧head。
在正常ESP-IDF终端打开controller工程，执行 `idf.py -p COM6 flash monitor`。
不执行erase-flash，不清除已保存的热点凭据。

也可直接烧录本次已经构建好的文件，避免旧Python环境再次触发编译：

```powershell
cd C:\Users\Administrator\Desktop\try_PZF\controller\build
& 'C:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' -m esptool --chip esp32p4 -p COM6 -b 460800 --before default_reset --after hard_reset write_flash '@flash_args'
& 'C:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' -m esp_idf_monitor -p COM6 -b 115200 controller.elf
```

这是供使用者执行的命令，本次没有代为执行。先关闭占用COM6的串口监视，再烧录；
第二条命令在烧录成功后打开监视。日志的ELF SHA256应以 `45f166b710` 开头，不能只看编译日期。

启动需看到 `Async uplink ready: depth=8 frame_max=1500`，且没有任务/队列分配失败。
记录启动后的内部RAM总量、最大块，不能以约30MB总堆代替内部RAM预算。

1. 正常唤醒后连续说两句话，完整听完TTS再说；验证STT及至少两轮回答。
2. 连续进行5～10分钟，包含TTS、结束对话、再次唤醒及本地播报。
3. 复现发送停顿时同时查看三行：
   - `[UPLINK_DIAG]`：send_try/done及inflight_ms指真实SDK调用，而非入队次数。
   - `[MIC_DIAG]`：发送在途等待时read/rx/enc仍推进，才支持采集/编码隔离成功。
   - `[UPLINK_QUEUE]`：queued始终0～8；过载时drop_oldest可增长，不应导致任务无限增长。
4. 单独验证唤醒词仍有响应。MIC计数推进本身不能证明唤醒识别正常；TTS期间继续遵守原有唤醒暂停策略。
5. 断线重连后说一条新指令，不能补发断线前积压的旧话；允许在途调用按原参数返回后才重建。
6. 多次重连后比较同一空闲阶段的Heap、内部RAM及sender_stack_free；稳定仅说明本次未见明显异常，
   不能单独证明无泄漏。栈余量与识别效果必须实测，8192只是首测值。

已知限制：丢旧音频会造成语音缺口；已进入SDK的旧帧无法取消；text的原有无限等待参数未修改。
本批保护本地采集，不承诺故障网络下仍能连续云端识别。

## 留存与回退

修改前文件位于 `version_snapshots/2026-10-19_controller_before_async_uplink/`，
按其中README恢复三个修改文件后重新编译、烧录。不要把恢复源码当成已恢复板上固件。
