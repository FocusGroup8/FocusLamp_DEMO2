# 拍头九条本地欢迎语

## 当前录音：小智云端音色离线资源（2026-10-22）

- 已将用户采集的九条欢迎语和一条久坐提醒 WAV 安装到 `main/audio`，不是微软慧慧音色。
- 声音来自该设备当时的普通小智云端对话；具体服务商/音色 ID 未知，不据此猜测型号。
- 仍是固定本地播放，不新增每次触发的云端请求。普通对话、轮播、合并待播、长按取消、
  雷达在位计时与播放互斥逻辑不变。只需要编译/烧录 controller，head 不动。
- 第四句现在是“今天辛苦了，休息一下，我会一直陪着你。”；第九句实际读的是“亮起来了”。
  所有实际云端字幕已同步到 `welcome_messages.json` 和播放日志文案。
- `main/audio/cloud_voice_assets.json` 记录全部十段的采集来源、实际字幕、SHA256、时长及采样数。
  采集目录仅用于溯源，不是编译依赖；构建只需要工程内的 WAV。
- 十段总计 1,147,020 字节，均为 16kHz、单声道、16bit PCM；直接使用采集文件，未再合成或重编码。
- 替换前资源、相关文案与旧 controller.bin 在
  `../version_snapshots/2026-10-22_before_cloud_voice_assets/`，可回退旧音色。
- 新资源尚未烧录，上板后检查欢迎语 1→9→1、长按取消、播后唤醒和久坐提醒。

### 本次电脑端验证（2026-10-22）

- 84 项 Python 测试通过；ESP-IDF 5.5.4 controller 构建成功。
- app 大小 `0x3d3fb0`，6 MiB app 分区剩余 `0x22c050`（约 36%）。
- 从新 ELF 中提取十段嵌入资源，SHA256 全部与采集 WAV 一致。
- 与替换前快照逐字节比较，播放函数实现、sdkconfig 和 dependencies.lock 未变；本批只更新录音与对应文案、来源清单、测试和说明。
- controller.bin SHA256：`18D14A1E713947D055C0AA01889EB37F4D222B4CC42B48D2D9CB8A0BE0AF0ED0`。
- controller.elf SHA256：`4AFC67D46E0096DC917EDBCE7CDFD4D0A16A9F86C48185DD1677BF83A5027E14`。
- 尚未烧录、未做扬声器试听；电脑端验证不替代上板检查。

## 本次行为

- 灯头原来的单击请求保留，基座将那三条固定欢迎文字识别为触发信号；实际播放基座内置的九条录音。
- 顺序 1→2→…→9→1，基座重启从第 1 条开始。编号在本条成功写出首个 PCM 块后推进；已出音频后被长按中断，下次播下一条。写出前被取消、等待扬声器锁失败的不跳号。
- 同一个本地播放任务处理欢迎语和原有雷达提醒，不重叠播放。连续拍击最多合并保留一条待播欢迎语，不按拍击次数无限排队。
- 长按沿用 `/api/chat/end`，先取消当前/待播欢迎语，再走原云端结束会话逻辑。取消是协作式的，不强删任务；当前 I2S 写入返回后清空 DMA 中的余音，不承诺按下瞬间静音。
- 欢迎语无需云端连接或正常对话结束；灯头到基座的局域网 HTTP 仍须正常。其他文字播报仍走原云端路径。
- 本地播报期间暂停唤醒和音频上行，丢弃云端扬声器音频，防止重叠与回声。播完或取消后成对释放本地暂停；仅在云端原有状态允许时恢复上传，不强制改成 LISTENING，不额外发送 listen/start。
- 正常云端说话时拍头会以本地录音占用扬声器，该段云端音频不会补播；本功能不提供播报中语音打断，长按仍可取消。

## 烧录

只编译、烧录 **controller / COM6**，head / COM4 不动。九条 WAV 嵌入 app 分区，不需额外刷文件系统、不擦 NVS。

在 controller 的 ESP-IDF 终端中执行：

```powershell
idf.py -p COM6 flash monitor
```

## 上板检查

1. 启动见 `Embedded PCM16 ready: reminder + 9 welcomes, 16kHz mono`。
2. 两板互连后，每次等播完再单击，听满十次：编号应为 1…9、1；日志为 `Welcome n/9 playback requested`，结束有 `pcm_started=1`，不是云端 `Speak cycle ... submitted`。
3. 播第一条时连续单击数次：当前条结束后最多追加一条，声音不重叠。停止拍击后不应继续排长队。
4. 播放时长按灯头：当前录音停止，待播清空；重新单击能播下一条。重启基座后从第 1 条开始。
5. 播完及长按取消后分别说唤醒词并正常对话，检查收音恢复。测试普通对话途中拍头：本地录音能启动，结束后仍可重新唤醒。
6. 雷达满足原有久坐条件时仍能播原提醒；双击亮度、长按关灯、光感、摄像头与机械臂按原方式回归。

`output finished: ESP_OK` 表示 PCM 与静音尾部已提交，不等于已用人耳确认音量、发音、扬声器或硬件时序正常。上述上板检查不能由编译及模型测试替代。

## 资产与复现

- 九条文字、文件名与格式记录在 `main/audio/welcome_messages.json`。
- 当前资源来自云端对话采集，流程见 `docs/tts_capture_usage.md`；换成新录音后需重新编译。
  `tools/generate_welcome_audio.ps1` 是早期 Windows SAPI 生成工具，不适用于当前云端音色。
  当前清单中的 voice 是来源说明，不是可传给 Windows `SelectVoice` 的本地音色名。
- 修改前留存：`../version_snapshots/2026-10-20_controller_before_local_welcome9`（相对于工程根目录）。
- 未改动 head、光感/摄像头/机械臂逻辑、sdkconfig、dependencies.lock、SDK 发送超时和心跳。

## 历史电脑端验证（2026-10-20，以下哈希/大小是替换前慧慧版）

- ESP-IDF 5.5.4 controller 完整构建成功，app 大小 `0x442540`（4,465,984 字节），6 MiB app 分区剩余 `0x1bdac0`，约 29%。九条录音合计 1,431,294 字节，直接嵌入 flash，不为九条录音新建任务或整段 RAM 缓冲。
- `python -m unittest discover -s controller/tests -v`：56 项通过（源码约束、独立 Python 策略模型、实际 WAV 文件检查）；不是 C/FreeRTOS 实机运行测试。
- `tools/test_local_welcome_policy.c` 使用 RISC-V GCC、`-std=c11 -O2 -Wall -Wextra -Werror -c` 编译通过；未在主机执行该 C fixture。
- head 原有 37 项测试通过；head bin、原久坐录音、demo_behaviors.c、sdkconfig 和 dependencies.lock 哈希与改动前一致。
- 新 controller.bin SHA256：`53980BE2C7B51FB1B3DD2768E161FE2E6264AE4ED9AF8DEBC3010F0FFF394889`。
- 新 controller.elf SHA256：`864513B214A1CEDD42171C9EE4A0EC9BD8A9BF7546BD17A39DD91DC75494515C`。
- 尚未烧录或做上板试听、实际长按响应和播后唤醒验证。
