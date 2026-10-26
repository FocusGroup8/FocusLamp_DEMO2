# 小智云端语音自动采集

本功能只保存设备实际收到的云端 Opus 回复，不调用未经确认的 TTS 接口、不发送文字注入。
仍需通过正常对话让小智朗读目标句子；自动化部分是划分回复、下载、转 WAV、字幕核对和编号。
拍头欢迎语/久坐提醒是本地声音，不会被误当成云端音色采集。

## 使用

1. 重新 Build controller，向基座 COM6 Flash；head 不需更改或烧录。
2. 电脑与基座连接同一网络，从基座日志 `[WiFi] Got IP` 获取 **controller IP**，不要用 head IP。
3. 在 controller 文件夹的 PowerShell 执行（把示例 IP 换成基座实际 IP）：

```powershell
.\tools\Capture-XiaozhiVoice.ps1 -ControllerIp '172.28.85.100'
```

脚本默认使用现有 ESP-IDF Python。FFmpeg 已准备在 `tools/.tts_capture_deps` 时无需再下载；
否则首次运行通过 pip 下载 `imageio-ffmpeg==0.6.0` 到该目录，不改系统 Python 环境。
已有 FFmpeg 可加 `-Ffmpeg 'D:\tools\ffmpeg.exe'`；Python 位置不同可加 `-PythonExe '...\python.exe'`。

4. 看到“已准备”后，通过正常语音对话说：

> 请只朗读这句话，不要添加其他内容：〔屏幕显示的目标句子〕

必要时先喊 focus。唤醒问候字幕与目标不符，会保存到 `unmatched` 并重新等待同一句。
云端字幕与目标一致，或命中采集文案表里允许的近义表达后，脚本自动保存并提示下一句，无需手动开始/停止录音。
总共九条欢迎语、一条久坐提醒。默认每句等待 180 秒，单次回复最多 60 秒/512KiB 编码数据。
Focus/福克斯视为同一名称。不会根据宽泛的相似度把任意问候或缺字残句当成目标。

5. 输出在 `controller/audio_captures/时间戳/`。每段有：
   - `.wav`：16kHz、单声道、16bit PCM，可用于现有本地播放；
   - `.opus`：直接保存的云端音频，不经过扬声器/麦克风二次录音；
   - `.fop`：原始有长度边界的 Opus 包，方便重新封装；
   - `.json`：云端实际字幕、目标文字、匹配类型（exact/equivalent/none）、采集编号及转换后时长。

字幕匹配不等于听感/实际发音完全正确，请逐条试听。文件不会自动覆盖 `main/audio`，也不会自动烧录。
确认正确后再替换十个同名 WAV，重新编译烧录 controller。普通云端对话音色配置不变。

只测试一段任意回复：

```powershell
.\tools\Capture-XiaozhiVoice.ps1 -ControllerIp '172.28.85.100' -Single
```

更多参数：`python tools/capture_xiaozhi_tts.py --help`。无 FFmpeg 时可加 `--opus-only` 先保存可播放的 Opus。
没有云端字幕时批量模式不会自动推进；可以用 Single 模式采集后人工核对。

已有第一句录音时，从第二句继续（仍输出到新目录，不覆盖之前已录文件）：

```powershell
.\tools\Capture-XiaozhiVoice.ps1 -ControllerIp '172.28.85.181' -StartAt 2
```

`-StartAt` 支持 1～10，10 是久坐提醒；不与 `-Single` 同用。

## 近义文案（2026-10-22）

用户允许意思相近后，采集专用文案改由 `tools/tts_capture_phrases.json` 提供，
不修改板端已有 WAV，也不修改 `main/audio/welcome_messages.json` 的原资源说明。
本次仅电脑脚本与文案配置更新，无需为此重新编译/烧录。

- 第四句提示简化为：**今天辛苦了，休息一下，我会一直陪着你。**
- 第二句的“加一啦/加一了”、第四句日志中的“先坐好/先做好”、
  “别着急/不要着急”等已列明的近义或同音字幕都可以接受。
- “我在呢”“你好”“拜拜”“今天”“辛苦了，亲”等无关问候/告别/残句不接受。
- 不改写实际录音和字幕；accepted 列表明确列出允许说法，匹配后记录 `match_type=equivalent`。
- 仍需试听确认发音与听感，字幕匹配不证明声音逐字准确。

前三句已录好时从第四句开始：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Users\Administrator\Desktop\try_PZF\controller\tools\Capture-XiaozhiVoice.ps1" -ControllerIp "172.28.85.181" -StartAt 4
```

日志若仍报 `capture_callback_busy`，是另一条旧固件错误路径；本次近义匹配不能修复它。
磁盘上的新采集模块已移除这条失败路径，但只有烧录了前一批修正版，板端才会更新。
本次近义匹配更新的电脑验证：82 项 controller 测试通过，覆盖明确近义变体、无关回复/残句拒绝、
原始字幕保留、近义表达完整批次和从指定编号续录。未改动或重新生成固件。

## 资源与关闭

- 默认关闭，不存历史对话；只有电脑工具明确 arm 才录下一段云端回复。
- 无新增 FreeRTOS 任务。512KiB PSRAM 在首次 arm 时分配，取消/工具正常退出时释放。
- 云端回调只用短临界区预留写入位置/登记在途计数，Opus 与字幕复制在临界区外完成。
  HTTP 状态查询只读取小元数据，完整字幕仅在冻结后复制，不再因状态查询抢锁报 callback_busy。
- STOP 后若还有已登记的写入，进入 sealing；写入实际退出后才 complete。
  在途写入未退出时，不允许下载、重新 arm 或释放缓冲，避免尾包未完成或悬空引用。
- 收到 TTS_STOP 才标记完整；断线、溢出或丢包不会生成“完整成功”的记录。
- 可按 Ctrl+C 退出，已有电脑文件保留。若电脑异常退出，手动取消释放缓存：

```powershell
Invoke-RestMethod -Uri 'http://172.28.85.100/api/tts/capture' -Method Post -ContentType 'application/json' -Body '{"action":"cancel"}'
```

录音和转换依赖目录已加入 controller/.gitignore。LAN 接口沿用工程现有的无认证 HTTP，
仅在可信局域网使用，不应做公网端口转发。电脑下载走直连 HTTP，不主动使用系统 HTTP 代理。

## 新接口（基座）

- `POST /api/tts/capture {"action":"arm"}`：准备下一段，返回 id；播放中/已有采集返回 409。
- `GET /api/tts/capture`：idle/armed/recording/sealing/complete/error、id、字幕、帧数及错误。
  录制中的字幕为空，完整录音封存后才提供完整字幕。
- `GET /api/tts/capture?download=1&id=N`：仅完整且编号相符时导出 FOP1 帧包。
- `POST /api/tts/capture {"action":"cancel"}`：释放缓存，不影响小智会话/播放。

## 验证边界与回退

电脑测试覆盖协议解析、Ogg 页、Opus 包时长、字幕匹配、模拟 HTTP 十句流水线。
FFmpeg 可用时另用真实 Opus 静音包验证 WAV 解码。它们不替代实机云端音色和试听验证。
固件编译成功也不代表已经烧录或获取到了真实云端录音。

2026-10-21 电脑检查：72 项 controller 测试通过（包含本机 HTTP 模拟十句流程、
真实 Opus 解码和损坏包拒绝），PowerShell 启动脚本语法检查通过；ESP-IDF 5.5.4 构建通过。
WAV 转换启用 FFmpeg `-xerror`，并核对实际样本数与捕获的 Opus 包时长，避免坏包静默跳过。
尚未烧录或采集实机声音。

2026-10-22 修正：实机发现 `capture_callback_busy`（HTTP 查询与回调共用互斥锁）。
本次仅调整采集模块的并发管理，以及 PC 的 sealing 状态和 StartAt 支持；不改普通小智语音、
雷达、头灯和现有本地录音。不再使用“回调一次抢锁失败就整段作废”的处理。
改前快照位于 `version_snapshots/2026-10-22_before_capture_contention_fix/`。
本次电脑验证：75 项 controller 测试通过（含 sealing 等待、StartAt=2 保持原编号、
旧录音不覆盖），PowerShell 语法检查及 ESP-IDF 5.5.4 构建通过。尚未烧录修正版；
需要在实机重新采集验证，不能以主机测试替代 FreeRTOS 并发和听感验收。

改前快照：`version_snapshots/2026-10-21_controller_before_tts_capture/`，包含原相关源码、sdkconfig、
dependencies.lock 和当时 controller.bin。回退源码需恢复快照文件、移除新增 tts_capture.c/.h 编译接入后重编译；
不需要回退 head 或修改云端账号。
